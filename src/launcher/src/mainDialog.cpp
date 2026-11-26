#include "mainDialog.h"

#include "configuration.h"
#include "cmake_config.h"
#include "configurationItem.h"
#include "configurationListWidget.h"
#include "controllerLightbar.h"
#include "gameContent.h"
#include "frameGenerationController.h"
#include "gt7Settings.h"
#include "launcherDiagnostics.h"
#include "patchesDialog.h"
#include "updateChecker.h"

#include <QApplication>
#include <QByteArray>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDialog>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIODevice>
#include <QLabel>
#include <QMessageBox>
#include <QObject>
#include <QPointer>
#include <QPixmap>
#include <QSignalBlocker>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QStringList>
#include <QTextStream>
#include <QUrl>
#include <QVariant>
#include <QtCore>

#include <cstdint>
#include <memory>

#include "ui_main_dialog.h"

#if defined(_WIN32)
#include <windows.h> // IWYU pragma: keep
#endif

// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <processthreadsapi.h>
// IWYU pragma: no_include <winbase.h>

class QWidget;

#if defined(_WIN32)
constexpr char EMULATOR_EXE[] = "kyty_emulator.exe";
#else
constexpr char EMULATOR_EXE[] = "kyty_emulator";
#endif

#if defined(_WIN32)
constexpr char CMD_EXE[] = "cmd.exe";
#elif defined(__linux__)
constexpr char KYTY_BASH_FILE[] = "kyty_run.sh";
#endif
#if defined(_WIN32)
constexpr DWORD CMD_X_CHARS = 175;
constexpr DWORD CMD_Y_CHARS = 1000;
#endif
constexpr char SETTINGS_MAIN_DIALOG[]        = "MainDialog";
constexpr char SETTINGS_MAIN_LAST_GEOMETRY[] = "geometry";
constexpr char SETTINGS_CHECK_UPDATES[]       = "check_updates_on_startup";
constexpr char SETTINGS_EXTERNAL_FG[]         = "external_frame_generation_enabled";

class MainDialogPrivate: public QObject {
	Q_OBJECT

public:
	explicit MainDialogPrivate(QObject* parent = nullptr): QObject(parent) {}
	~MainDialogPrivate() override;

	void Setup(MainDialog* main_dialog);

	/*slots:*/

	void Update();
	void FindInterpreter();
	void Run();
	void ExportDiagnostics(const QString& run_directory);
	void ShowAbnormalExit(int exit_code, QProcess::ExitStatus exit_status,
	                      const QString& run_directory);

	[[nodiscard]] const QString& GetInterpreter() const { return m_interpreter; }
	[[nodiscard]] bool IsGameRunning() const { return m_process.state() != QProcess::NotRunning; }

	static void WriteSettings(QSettings& s);
	static void ReadSettings(QSettings& s);

private:
	static QByteArray g_last_geometry;
	static bool       g_check_updates_on_startup;
	static bool       g_external_fg_enabled;

	Ui::MainDialog* m_ui             = {nullptr};
	MainDialog*     m_main_dialog    = nullptr;
	UpdateChecker*  m_update_checker = nullptr;
	FrameGenerationController* m_frame_generation = nullptr;
	bool m_running_gt7 = false;
	ControllerLightbar m_lightbar;
	QString         m_interpreter;

	QProcess m_process;
	std::unique_ptr<LauncherDiagnostics::Run> m_diagnostic_run;
	QLabel*       m_logs_path_label    = nullptr;
	QPushButton*  m_open_logs_button   = nullptr;
	QPushButton*  m_copy_log_button    = nullptr;
	QPushButton*  m_export_logs_button = nullptr;

	QPointer<ConfigurationItem> m_running_item;
};

QByteArray MainDialogPrivate::g_last_geometry;
bool       MainDialogPrivate::g_check_updates_on_startup = true;
bool       MainDialogPrivate::g_external_fg_enabled = true;

MainDialog::MainDialog(QWidget* parent): QDialog(parent), m_p(new MainDialogPrivate(this)) {
	m_p->Setup(this);
}

MainDialogPrivate::~MainDialogPrivate() {
	if (m_frame_generation != nullptr) m_frame_generation->Stop();
	m_process.disconnect(this);
	delete m_ui;
}

void MainDialogPrivate::ExportDiagnostics(const QString& run_directory) {
	if (run_directory.isEmpty()) {
		QMessageBox::information(m_main_dialog, tr("Export diagnostics"),
		                         tr("No GT7 run log is available yet."));
		return;
	}
	auto output_dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
	if (output_dir.isEmpty()) {
		output_dir = QFileInfo(LauncherDiagnostics::LogsRoot()).absolutePath();
	}
	const auto suggested = QDir(output_dir).absoluteFilePath(
	    QStringLiteral("GT7-diagnostics-%1.zip").arg(QFileInfo(run_directory).fileName()));
	auto output = QFileDialog::getSaveFileName(m_main_dialog, tr("Export diagnostics"), suggested,
	                                           tr("ZIP archive (*.zip)"));
	if (output.isEmpty()) {
		return;
	}
	if (!output.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
		output += QStringLiteral(".zip");
	}
	QString error;
	if (!LauncherDiagnostics::ExportRunBundle(run_directory, output, &error)) {
		QMessageBox::critical(m_main_dialog, tr("Export diagnostics"), error);
		return;
	}
	QApplication::clipboard()->setText(QDir::toNativeSeparators(output));
	QMessageBox::information(
	    m_main_dialog, tr("Diagnostics exported"),
	    tr("The diagnostic package was saved and its path was copied to the clipboard:\n%1\n\n"
	       "Nothing was uploaded. Known user, game and emulator paths were removed. Review the "
	       "remaining text before sharing it.")
	        .arg(QDir::toNativeSeparators(output)));
}

void MainDialogPrivate::ShowAbnormalExit(int exit_code, QProcess::ExitStatus exit_status,
	                                      const QString& run_directory) {
	if (exit_status == QProcess::NormalExit && exit_code == 0) {
		return;
	}
	const auto hex_code =
	    QString::number(static_cast<quint32>(exit_code), 16).rightJustified(8, QLatin1Char('0'));
	QMessageBox dialog(
	    QMessageBox::Critical, tr("GT7 ended unexpectedly"),
	    tr("GT7 ended unexpectedly.\n\nExit code: %1 (0x%2)\nLog folder:\n%3\n\n"
	       "Open this run's log folder or export a diagnostic ZIP for analysis.")
	        .arg(exit_code)
	        .arg(hex_code.toUpper(), QDir::toNativeSeparators(run_directory)),
	    QMessageBox::Close, m_main_dialog);
	auto* open_button = dialog.addButton(tr("Open log folder"), QMessageBox::ActionRole);
	auto* export_button =
	    dialog.addButton(tr("Export diagnostics..."), QMessageBox::ActionRole);
	dialog.exec();
	if (dialog.clickedButton() == open_button) {
		if (!QDesktopServices::openUrl(QUrl::fromLocalFile(run_directory))) {
			QMessageBox::warning(m_main_dialog, tr("Diagnostic logs"),
			                     tr("Could not open the log folder:\n%1").arg(run_directory));
		}
	} else if (dialog.clickedButton() == export_button) {
		ExportDiagnostics(run_directory);
	}
}

void MainDialogPrivate::Setup(MainDialog* main_dialog) {
	m_ui = new Ui::MainDialog;
	m_ui->setupUi(main_dialog);
	main_dialog->setWindowTitle(QStringLiteral("Tohi-GT7 " KYTY_VERSION));
	m_ui->gt7_release_badge->setText(QStringLiteral(KYTY_VERSION));
	m_ui->gt7_feature_panel->SetArtwork(QStringLiteral(":/images/gt7-super-gt.jpg"), 0.66);
	m_ui->graphics_brand_panel->SetArtwork(QStringLiteral(":/images/gt7-official-hero.jpg"), 0.6);
	const auto set_wordmark = [](QLabel* label, double logical_width) {
		QPixmap logo(QStringLiteral(":/images/gt7-official-logo.png"));
		if (!logo.isNull()) logo.setDevicePixelRatio(logo.width() / logical_width);
		label->setPixmap(logo);
	};
	set_wordmark(m_ui->gt7_brand_logo, 300);
	set_wordmark(m_ui->graphics_brand_logo, 216);
	m_ui->gt7_feature_panel->setToolTip(tr("Official Gran Turismo 7 promotional artwork"));
	m_ui->graphics_brand_panel->setToolTip(tr("Official Gran Turismo 7 promotional artwork"));

	m_main_dialog = main_dialog;
	m_update_checker = new UpdateChecker(main_dialog);

	main_dialog->setWindowFlags(Qt::Window | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
	auto* navigation = new QButtonGroup(main_dialog);
	const QList<QPushButton*> buttons {m_ui->nav_games, m_ui->nav_graphics,
	                                  m_ui->nav_settings, m_ui->nav_help};
	for (int index = 0; index < buttons.size(); ++index) {
		navigation->addButton(buttons.at(index), index);
	}
	m_ui->nav_games->setChecked(true);
	connect(navigation, &QButtonGroup::idClicked, m_ui->pages, &QStackedWidget::setCurrentIndex);
	connect(m_ui->settings_global_button, &QPushButton::clicked, m_ui->widget,
	        &ConfigurationListWidget::edit_global_settings);
	connect(m_ui->settings_game_button, &QPushButton::clicked, m_ui->widget,
	        &ConfigurationListWidget::edit_configuration);
	connect(m_ui->settings_input_button, &QPushButton::clicked, m_ui->widget,
	        &ConfigurationListWidget::edit_input_mapping);
	connect(m_ui->settings_wheel_button, &QPushButton::clicked, m_ui->widget,
	        &ConfigurationListWidget::edit_wheel);
	m_frame_generation = new FrameGenerationController(this);
	connect(m_frame_generation, &FrameGenerationController::StatusChanged, this,
	        [this](const QString& text, bool active) {
		        m_ui->game_fg_status->setText(text);
		        m_ui->game_fg_status->setProperty("frameGenerationActive", active);
	        });
	m_frame_generation->setEnabled(g_external_fg_enabled);
	m_ui->external_fg_checkbox->setChecked(g_external_fg_enabled);
	connect(m_ui->external_fg_checkbox, &QCheckBox::toggled, this, [this](bool enabled) {
		g_external_fg_enabled = enabled;
		m_frame_generation->setEnabled(enabled);
		m_ui->widget->WriteSettings();
		Update();
	});

	connect(main_dialog, &MainDialog::Start, this, &MainDialogPrivate::FindInterpreter,
	        Qt::QueuedConnection);
	connect(m_ui->widget, &ConfigurationListWidget::Select, this, &MainDialogPrivate::Update);
	connect(m_ui->widget, &ConfigurationListWidget::PreviewControllerColor, this,
	        [this](const QString& color) {
		        if (m_process.state() == QProcess::NotRunning) {
			        m_lightbar.SetColor(color);
		        }
	        });
	connect(m_ui->widget, &ConfigurationListWidget::Run, this, &MainDialogPrivate::Run);
	connect(m_ui->gt7_start_button, &QPushButton::clicked, this, &MainDialogPrivate::Run);
	m_ui->gt7_dlss_checkbox->setChecked(m_ui->widget->Gt7DlssEnabled());
	connect(m_ui->gt7_dlss_checkbox, &QCheckBox::toggled, m_ui->widget,
	        &ConfigurationListWidget::SetGt7DlssEnabled);
	if (!qEnvironmentVariableIsSet("KYTY_LAUNCHER_UI_SMOKE")) {
		QDir().mkpath(LauncherDiagnostics::LogsRoot());
	}
	m_logs_path_label = new QLabel(
	    tr("Diagnostic logs: %1").arg(QDir::toNativeSeparators(LauncherDiagnostics::LogsRoot())),
	    main_dialog);
	m_logs_path_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
	m_logs_path_label->setWordWrap(true);
	m_ui->verticalLayout_4->insertWidget(0, m_logs_path_label);
	m_ui->label_settings_file->setTextInteractionFlags(Qt::TextSelectableByMouse);
	auto* support_layout = new QHBoxLayout;
	m_open_logs_button   = new QPushButton(tr("Open log folder"), main_dialog);
	m_copy_log_button    = new QPushButton(tr("Copy latest log path"), main_dialog);
	m_export_logs_button = new QPushButton(tr("Export diagnostics..."), main_dialog);
	m_open_logs_button->setAutoDefault(false);
	m_copy_log_button->setAutoDefault(false);
	m_export_logs_button->setAutoDefault(false);
	m_open_logs_button->setToolTip(tr("Open the folder containing one timestamped directory for each GT7 run."));
	m_copy_log_button->setToolTip(tr("Copy the latest GT7 run log directory to the clipboard."));
	m_export_logs_button->setToolTip(
	    tr("Create a local ZIP with logs and a run summary. No file is uploaded automatically."));
	support_layout->addWidget(m_open_logs_button);
	support_layout->addWidget(m_copy_log_button);
	support_layout->addWidget(m_export_logs_button);
	support_layout->addStretch();
	m_ui->verticalLayout_4->insertLayout(1, support_layout);
	connect(m_open_logs_button, &QPushButton::clicked, this, [this]() {
		const auto root = LauncherDiagnostics::LogsRoot();
		QDir().mkpath(root);
		if (!QDesktopServices::openUrl(QUrl::fromLocalFile(root))) {
			QMessageBox::warning(m_main_dialog, tr("Diagnostic logs"),
			                     tr("Could not open the log folder:\n%1").arg(root));
		}
	});
	connect(m_copy_log_button, &QPushButton::clicked, this, []() {
		const auto latest = LauncherDiagnostics::LatestRunDirectory();
		if (latest.isEmpty()) {
			return;
		}
		QApplication::clipboard()->setText(QDir::toNativeSeparators(latest));
	});
	connect(m_export_logs_button, &QPushButton::clicked, this, [this]() {
		ExportDiagnostics(LauncherDiagnostics::LatestRunDirectory());
	});
	connect(main_dialog, &MainDialog::Resize, [this]() {
		m_ui->gt7_feature_panel->setFixedHeight(m_main_dialog->height() < 700 ? 200 : 220);
		g_last_geometry = m_main_dialog->saveGeometry();
		m_ui->widget->WriteSettings();
	});

	connect(&m_process, &QProcess::stateChanged, this, [this](QProcess::ProcessState state) {
		if (state == QProcess::NotRunning) {
			if (m_running_item != nullptr) {
				m_running_item->SetRunning(false);
			}
		}
		Update();
	});
	connect(&m_process, &QProcess::started, this, [this]() {
		if (m_diagnostic_run != nullptr) {
			m_diagnostic_run->RecordStarted(m_process.processId());
		}
		if (m_running_gt7) {
			m_frame_generation->Start(m_process.processId(),
			    m_diagnostic_run != nullptr ? m_diagnostic_run->Directory() : QString {});
		}
	});
	connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
		if (m_diagnostic_run != nullptr) {
			m_diagnostic_run->RecordProcessError(static_cast<int>(error), m_process.errorString());
		}
	});
	connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
	        [this](int exit_code, QProcess::ExitStatus exit_status) {
		        m_frame_generation->Stop();
		        m_running_gt7 = false;
		        QString run_directory;
		        if (m_diagnostic_run != nullptr) {
			        m_diagnostic_run->RecordFinished(exit_code,
			                                         exit_status == QProcess::NormalExit);
			        run_directory = m_diagnostic_run->Directory();
		        }
		        if (!run_directory.isEmpty()) {
			        ShowAbnormalExit(exit_code, exit_status, run_directory);
		        }
	        });

	m_ui->label_settings_file->setText(tr("Settings file: ") + m_ui->widget->GetSettingsFile());

	m_main_dialog->restoreGeometry(g_last_geometry);

	Update();
}

void MainDialogPrivate::FindInterpreter() {
	QDir search_dir(QApplication::applicationDirPath());
	m_interpreter = search_dir.absoluteFilePath(EMULATOR_EXE);

	if (!QFile::exists(m_interpreter)) {
		search_dir.cdUp();
		m_interpreter = search_dir.absoluteFilePath(EMULATOR_EXE);
	}

	bool found = QFile::exists(m_interpreter);
	m_ui->widget->SetRuntimeDirectory(QFileInfo(m_interpreter).absolutePath());

	if (found) {

		QProcess test;
		test.setProgram(m_interpreter);
		test.start();
		test.waitForFinished();

		auto output = QString(test.readAllStandardOutput());
		auto lines  = output.split(QRegularExpression("[\r\n]"), Qt::SkipEmptyParts);

		if (lines.count() < 2) {
			found = false;
		}
	}

	if (!found) {
		QMessageBox::critical(m_main_dialog, tr("Error"), tr("Can't find emulator"));
		QApplication::quit();
		return;
	}

	// Prompt for a game folder when none are configured, but keep the launcher
	// open if the user dismisses the dialog (quitting here can segfault during
	// nested modal shutdown / background compatibility load).
	m_ui->widget->EnsureGameDirectory();

	m_ui->label_settings_file->setText(tr("Settings file: ") + m_ui->widget->GetSettingsFile());

	Update();
	if (g_check_updates_on_startup) {
		m_update_checker->Check(false);
	}
}

static QString BoolArg(bool value) {
	return value ? QStringLiteral("true") : QStringLiteral("false");
}

static QStringList CreateEmulatorArgs(const Configuration& info,
                                      const QString& forced_printf_file = {}) {
	QStringList args;
	auto        r = EnumToText(info.screen_resolution).split('x');

	if (r.size() != 2) {
		return {};
	}

	args << "--screen-width" << r.at(0);
	args << "--screen-height" << r.at(1);
	args << "--user-name" << info.user_name;
	args << "--user-id" << QString::number(info.user_id);
	if (!info.audio_input_device.isEmpty()) {
		args << "--mic" << info.audio_input_device;
	}
	if (!info.controller.color.isEmpty()) {
		args << "--controller-color" << info.controller.color;
	}
	args << "--controller-volume" << QString::number(info.controller.speaker_volume);
	args << "--controller-vibration" << QString::number(info.controller.vibration_intensity);
	args << "--present-mode" << EnumToText(info.present_mode);
	if (info.gpu_index >= 0) {
		args << "--gpu" << QString::number(info.gpu_index);
	}
	if (info.fullscreen_enabled) {
		args << "--fullscreen";
	}
	if (info.hide_cursor_enabled) {
		args << "--hide-cursor";
	}
	args << "--readback-linear-images" << BoolArg(info.readback_linear_images);
	args << "--trophy-notifications" << BoolArg(info.trophy_enabled);
	if (info.tessellation_enabled) {
		args << "--tessellation";
	}
	if (info.bindless_images_enabled) {
		args << "--bindless";
	}
	if (info.reflections_half_rate) {
		args << "--reflections-every" << QStringLiteral("2");
	}
	if (!info.skip_shader_hashes.isEmpty()) {
		args << "--skip-shaders" << info.skip_shader_hashes;
	}
	args << "--vblank-frequency" << QString::number(info.vblank_frequency);
	args << "--console-language" << QString::number(info.console_language);
	args << "--vulkan-validation" << BoolArg(info.vulkan_validation_enabled);
	args << "--shader-validation" << BoolArg(info.shader_validation_enabled);
	args << "--shader-optimization-type" << EnumToText(info.shader_optimization_type);
	args << "--shader-log-direction" << EnumToText(info.shader_log_direction);
	args << "--shader-log-folder" << info.shader_log_folder;
	args << "--command-buffer-dump" << BoolArg(info.command_buffer_dump_enabled);
	args << "--command-buffer-dump-folder" << info.command_buffer_dump_folder;
	args << "--printf-direction"
	     << (forced_printf_file.isEmpty() ? EnumToText(info.printf_direction)
	                                      : QStringLiteral("File"));
	args << "--printf-output-file"
	     << (forced_printf_file.isEmpty() ? info.printf_output_file : forced_printf_file);
	if (info.profiler_enabled) {
		args << "--profile";
	}
	args << "--spirv-debug-printf" << "false";
	if (info.amd_cpu_enabled) {
		args << "--amd-cpu";
	}
#if defined(_WIN32)
	// The emulator protects the red zone unless told not to (round 149).
	args << (info.red_zone_protection_enabled ? "--redzone" : "--no-redzone");
#endif
	for (const auto& binding: info.host_input_mapping) {
		args << "--keymap" << binding;
	}
	if (info.controller.wheel_enabled && !info.controller.wheel_config.isEmpty()) {
		args << "--wheel" << BoolArg(true);
		args << "--wheel-config" << info.controller.wheel_config;
	}
	if (info.renderdoc_enabled) {
		args << "--rd";
	}

	QString game = info.basedir;
	if (!info.elf.isEmpty() && !GameContent::IsArchive(info.basedir)) {
		game = QDir(info.basedir).filePath(info.elf);
	}
	args << "--game" << game;
	args << Gt7ProfileArguments(info);

	const auto patch_plan = PatchesDialog::PatchPlanPath(info.title_id);
	if (QFileInfo::exists(patch_plan)) {
		args << "--game-patch" << patch_plan;
	}

	return args;
}

#ifdef __linux__
static QString BashQuote(QString value) {
	value.replace('\'', "'\\''");
	return QStringLiteral("'") + value + QStringLiteral("'");
}

static bool CreateBashScript(const QString& interpreter, const QStringList& args,
                             const QString& file_name) {
	QFile file(file_name);
	if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		QTextStream s(&file);

		s << "#!/bin/bash\n";
		s << BashQuote(interpreter);
		for (const auto& arg: args) {
			s << " " << BashQuote(arg);
		}
		s << "\n";
		s << "echo Press any key...\n";
		s << "read -n1\n";

		file.close();

		return file.setPermissions(file.permissions() | QFile::ExeUser | QFile::ExeOwner |
		                           QFile::ExeGroup);
	}
	return false;
}

// Find a terminal and its command separator.
static bool FindTerminal(QString* program, QStringList* prefix) {
	struct TerminalSpec {
		const char* executable;
		const char* separator; // nullptr when the command follows immediately
	};

	static const TerminalSpec candidates[] = {
	    {"x-terminal-emulator", "-e"},
	    {"gnome-terminal", "--"},
	    {"konsole", "-e"},
	    {"xfce4-terminal", "-x"},
	    {"mate-terminal", "--"},
	    {"tilix", "-e"},
	    {"alacritty", "-e"},
	    {"kitty", nullptr},
	    {"foot", nullptr},
	    {"wezterm", "-e"},
	    {"urxvt", "-e"},
	    {"xterm", "-e"},
	};

	const auto try_candidate = [program, prefix](const QString& executable, const char* separator) {
		const auto resolved = QStandardPaths::findExecutable(executable);
		if (resolved.isEmpty()) {
			return false;
		}
		*program = resolved;
		prefix->clear();
		if (separator != nullptr) {
			*prefix << QString::fromLatin1(separator);
		}
		return true;
	};

	if (const auto from_env = qEnvironmentVariable("TERMINAL"); !from_env.isEmpty()) {
		// Reuse the known separator for an explicit terminal.
		const auto  env_name  = QFileInfo(from_env).fileName();
		const char* separator = "-e";
		for (const auto& candidate: candidates) {
			if (env_name == QLatin1String(candidate.executable)) {
				separator = candidate.separator;
				break;
			}
		}
		if (try_candidate(from_env, separator)) {
			return true;
		}
	}

	for (const auto& candidate: candidates) {
		if (try_candidate(QString::fromLatin1(candidate.executable), candidate.separator)) {
			return true;
		}
	}

	return false;
}
#endif

#if defined(_WIN32)
// Quote one token for cmd.exe so paths with spaces survive /K parsing.
static QString WinCmdQuote(QString value) {
	value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
	return QLatin1Char('"') + value + QLatin1Char('"');
}

static QString BuildWinCmdKCommand(const QString& interpreter, const QStringList& args) {
	QString command = WinCmdQuote(QDir::toNativeSeparators(interpreter));
	for (const auto& arg: args) {
		command += QLatin1Char(' ');
		command += WinCmdQuote(arg);
	}
	return command;
}
#endif

void MainDialog::RunInterpreter(QProcess* process, const Configuration& requested_info,
                                LauncherDiagnostics::Run* diagnostics) {
	const auto& interpreter = m_p->GetInterpreter();
	Configuration info;
	info.CopyGameInfoFrom(requested_info);
	info.CopyEmulatorSettingsFrom(requested_info);
	info.controller = requested_info.controller;
	info.basedir = requested_info.basedir;
	info.elf = requested_info.elf;
	ApplyGt7DesktopPreset(info);

	QFileInfo f(interpreter);
	auto      dir = f.absoluteDir();
	const bool gt7_title =
	    info.title_id.compare(QStringLiteral("PPSA01317"), Qt::CaseInsensitive) == 0;
	const bool gt7_profile = HasGt7Profile(info);
	if (gt7_profile && QFileInfo::exists(PatchesDialog::PatchPlanPath(info.title_id))) {
		if (diagnostics != nullptr) {
			diagnostics->RecordRejected(QStringLiteral("patch_conflict"),
			                            QStringLiteral("A custom game patch plan is enabled."));
		}
		QMessageBox::critical(
		    this, tr("GT7 profile conflict"),
		    tr("Disable the custom game patches before using the GT7 graphics preset. "
		       "These profiles require their own verified patch plan."));
		return;
	}

	const auto environment = Gt7LaunchEnvironment(info, dir.absolutePath(),
	                                               QProcessEnvironment::systemEnvironment());
	process->setProcessEnvironment(environment);
	if (gt7_title && info.gt7_dlss_enabled) {
		const auto missing = MissingGt7GraphicsRuntimeFiles(dir.absolutePath());
		if (!missing.isEmpty()) {
			const auto detail = tr("The PSSR 4K preset needs the complete Tohi-GT7 release package.\n"
			                       "Missing files: %1\n\nExtract the full package, or turn off the graphics preset.")
			                        .arg(missing.join(QStringLiteral(", ")));
			if (diagnostics != nullptr) {
				diagnostics->RecordRejected(QStringLiteral("graphics_runtime"), detail);
			}
			QMessageBox::critical(this, tr("Graphics runtime missing"), detail);
			return;
		}
	}
	auto args = CreateEmulatorArgs(
	    info, diagnostics != nullptr && !gt7_title
	              ? diagnostics->EmulatorLogPath() : QString {});
	if (args.isEmpty()) {
		if (diagnostics != nullptr) {
			diagnostics->RecordRejected(QStringLiteral("configuration"),
			                            QStringLiteral("Invalid emulator configuration."));
		}
		QMessageBox::critical(this, tr("Error"), tr("Invalid emulator configuration"));
		QApplication::quit();
		return;
	}
	if (gt7_profile) {
		// The same strict core check protects both this UI and direct CLI users. This process
		// exits before loading the game, creating caches, or initializing SteamVR.
		QProcess check;
		check.setProgram(interpreter);
		check.setArguments(args + QStringList {QStringLiteral("--gt7-check-only")});
		check.setWorkingDirectory(dir.path());
		check.setProcessEnvironment(environment);
		QProgressDialog progress(tr("Checking GT7 version and patch compatibility..."),
		                         tr("Cancel"), 0, 0, this);
		progress.setWindowModality(Qt::WindowModal);
		progress.setMinimumDuration(0);
		progress.show();
		QEventLoop loop;
		QTimer timeout;
		timeout.setSingleShot(true);
		bool interrupted = false;
		connect(&check, &QProcess::finished, &loop, &QEventLoop::quit);
		connect(&check, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
		const auto cancel = [&]() {
			interrupted = true;
			check.kill();
			loop.quit();
		};
		connect(&progress, &QProgressDialog::canceled, &loop, cancel);
		connect(&timeout, &QTimer::timeout, &loop, cancel);
		check.start();
		timeout.start(30000);
		loop.exec();
		timeout.stop();
		if (check.state() != QProcess::NotRunning) {
			check.kill();
			check.waitForFinished(3000);
		}
		// Programmatic close emits canceled too; it must not turn a successful check into
		// a user cancellation and prevent the game from starting.
		disconnect(&progress, nullptr, &loop, nullptr);
		progress.close();
		const auto check_stdout = check.readAllStandardOutput();
		const auto check_stderr = check.readAllStandardError();
		if (diagnostics != nullptr) {
			diagnostics->RecordPreflight(
			    check_stdout, check_stderr, check.exitCode(),
			    check.exitStatus() == QProcess::NormalExit,
			    check.error() == QProcess::UnknownError ? QString() : check.errorString(),
			    interrupted);
		}
		if (interrupted) {
			return;
		}
		if (check.exitStatus() != QProcess::NormalExit || check.exitCode() != 0 ||
		    check.error() == QProcess::FailedToStart) {
			auto detail =
			    QString::fromUtf8(check_stderr) + QString::fromUtf8(check_stdout);
			if (detail.isEmpty()) {
				detail = check.errorString();
			}
			if (diagnostics != nullptr) {
				detail += tr("\n\nLog folder:\n%1")
				              .arg(QDir::toNativeSeparators(diagnostics->Directory()));
			}
			QMessageBox::critical(this, tr("GT7 compatibility check failed"),
			                      detail);
			return;
		}
	}

	if (gt7_title) {
		// QProcess quotes individual arguments. Do not pass user-selected game paths through
		// cmd.exe, where percent expansion can change even a double-quoted path.
		process->setProgram(interpreter);
		process->setArguments(args);
#if defined(_WIN32)
		process->setNativeArguments({});
#endif
	} else {
#ifdef __linux__
	auto bash_file_name = dir.filePath(KYTY_BASH_FILE);
	if (!CreateBashScript(interpreter, args, bash_file_name)) {
		QMessageBox::critical(this, tr("Error"), tr("Can't create file:\n") + bash_file_name);
		QApplication::quit();
		return;
	}

	{
		QString     terminal;
		QStringList terminal_prefix;
		// Pass the script as a file argument (not bash -c) so paths with spaces work.
		if (FindTerminal(&terminal, &terminal_prefix)) {
			process->setProgram(terminal);
			process->setArguments(terminal_prefix + QStringList {"bash", bash_file_name});
		} else {
			// Run without a terminal as a fallback.
			process->setProgram(QStringLiteral("bash"));
			process->setArguments({bash_file_name});
		}
	}
#elif defined(_WIN32)
	{
		// Use nativeArguments so Qt does not re-quote the /K command string.
		process->setProgram(CMD_EXE);
		process->setArguments({});
		// Round 134: the console then shows the emulator's exit code, also after an end without
		// a crash report (a crash inside the graphics driver: the log just stops). "call" expands
		// %errorlevel% after the emulator has run, not when the line is read.
		process->setNativeArguments(
		    QStringLiteral("/K \"") + BuildWinCmdKCommand(interpreter, args) +
		    QStringLiteral(
		        " & echo. & call echo The emulator ended with exit code %^errorlevel%\""));
	}
#else
	process->setProgram(interpreter);
	process->setArguments(args);
#endif
	}
	process->setWorkingDirectory(dir.path());
	process->setStandardOutputFile(QString {});
	process->setStandardErrorFile(QString {});
	if (diagnostics != nullptr) {
		diagnostics->ConfigureProcess(process);
	}
#if defined(_WIN32)
	process->setCreateProcessArgumentsModifier([capture_logs = diagnostics != nullptr](
	                                               QProcess::CreateProcessArguments* args) {
		if (!capture_logs) {
			args->flags |= static_cast<uint32_t>(CREATE_NEW_CONSOLE);
			args->startupInfo->dwFlags &= ~static_cast<DWORD>(STARTF_USESTDHANDLES);
		}
		args->startupInfo->dwFlags |= static_cast<DWORD>(STARTF_USECOUNTCHARS);
		args->startupInfo->dwXCountChars = CMD_X_CHARS;
		args->startupInfo->dwYCountChars = CMD_Y_CHARS;
		// args->startupInfo->dwFlags |= static_cast<DWORD>(STARTF_USEFILLATTRIBUTE);
		// args->startupInfo->dwFillAttribute =
		//     static_cast<DWORD>(BACKGROUND_BLUE) | static_cast<DWORD>(FOREGROUND_RED) |
		//     static_cast<DWORD>(FOREGROUND_INTENSITY);
	});
#endif
	process->start();
	// Report immediate launch failures. A GT7 run has no separate console because its output is
	// preserved in the timestamped diagnostic directory.
#if !defined(_WIN32)
	const bool report_start_failure = true;
#else
	const bool report_start_failure = diagnostics != nullptr;
#endif
	if (report_start_failure && !process->waitForStarted(5000)) {
		if (diagnostics != nullptr) {
			diagnostics->RecordRejected(QStringLiteral("start"), process->errorString());
		}
		auto detail = tr("Failed to start:\n%1\n\n%2")
		                  .arg(process->program(), process->errorString());
		if (diagnostics != nullptr) {
			detail += tr("\n\nLog folder:\n%1")
			              .arg(QDir::toNativeSeparators(diagnostics->Directory()));
		}
		QMessageBox::critical(
		    this, tr("Error"), detail);
		return;
	}
	process->waitForFinished(100);
}

void MainDialog::WriteSettings(QSettings& s) {
	MainDialogPrivate::WriteSettings(s);
}

void MainDialog::ReadSettings(QSettings& s) {
	MainDialogPrivate::ReadSettings(s);
}

void MainDialog::resizeEvent(QResizeEvent* event) {
	emit Resize();
	QDialog::resizeEvent(event);
}

void MainDialog::closeEvent(QCloseEvent* event) {
	if (m_p->IsGameRunning()) {
		// Keep tracking the game: destroying its QProcess would terminate it.
		event->ignore();
		showMinimized();
		return;
	}
	QDialog::closeEvent(event);
}

void MainDialog::reject() {
	// Escape calls reject directly, so it needs the same protection as the title-bar close button.
	if (m_p->IsGameRunning()) {
		showMinimized();
		return;
	}
	QDialog::reject();
}

void MainDialogPrivate::WriteSettings(QSettings& s) {
	s.beginGroup(SETTINGS_MAIN_DIALOG);

	if (!g_last_geometry.isEmpty()) {
		s.setValue(SETTINGS_MAIN_LAST_GEOMETRY, g_last_geometry);
	}
	s.setValue(SETTINGS_CHECK_UPDATES, g_check_updates_on_startup);
	s.setValue(SETTINGS_EXTERNAL_FG, g_external_fg_enabled);

	s.endGroup();
}

void MainDialogPrivate::ReadSettings(QSettings& s) {
	s.beginGroup(SETTINGS_MAIN_DIALOG);

	g_last_geometry = s.value(SETTINGS_MAIN_LAST_GEOMETRY, g_last_geometry).toByteArray();
	g_check_updates_on_startup = s.value(SETTINGS_CHECK_UPDATES, true).toBool();
	g_external_fg_enabled = s.value(SETTINGS_EXTERNAL_FG, true).toBool();

	s.endGroup();
}

void MainDialogPrivate::Run() {
	if (qEnvironmentVariableIsSet("KYTY_LAUNCHER_UI_SMOKE") ||
	    m_process.state() != QProcess::NotRunning) {
		return;
	}
	m_running_item = m_ui->widget->GetSelectedItem();
	if (m_running_item == nullptr) {
		return;
	}

	m_running_item->SetRunning(true);
	m_lightbar.Stop();

	auto info = m_ui->widget->CreateConfiguration(*m_running_item);
	ApplyGt7DesktopPreset(*info);
	const bool gt7_title =
	    info->title_id.compare(QStringLiteral("PPSA01317"), Qt::CaseInsensitive) == 0;
	m_running_gt7 = gt7_title;
	if (gt7_title) {
		LauncherDiagnostics::RunDetails details;
		details.title_id          = info->title_id;
		details.game_version      = info->gameVersion;
		details.output_resolution = EnumToText(info->screen_resolution);
		details.mode = info->gt7_dlss_enabled ? QStringLiteral("Native PSSR inputs / DLSS 1440p to 4K")
		                                      : QStringLiteral("Desktop");
		details.mode += g_external_fg_enabled
		                    ? QStringLiteral("; external capture FG 2X requested")
		                    : QStringLiteral("; external FG off");
		details.mode += QStringLiteral("; built-in FG off; no extra FPS cap");
		details.desktop_height = info->gt7_desktop_height;
		details.vr_scale       = info->gt7_vr_scale;
		details.gpu_index      = info->gpu_index;
		details.emulator_path  = m_interpreter;
		details.private_paths  = {info->basedir, QFileInfo(m_interpreter).absolutePath()};
		QString diagnostic_error;
		m_diagnostic_run = LauncherDiagnostics::Run::Create(details, &diagnostic_error);
		if (m_diagnostic_run == nullptr) {
			QMessageBox::warning(
			    m_main_dialog, tr("Diagnostic logs"),
			    tr("The game can still be started, but this run could not create its diagnostic "
			       "folder:\n%1")
			        .arg(diagnostic_error));
		}
	} else {
		m_diagnostic_run.reset();
	}
	m_main_dialog->RunInterpreter(&m_process, *info, m_diagnostic_run.get());
	if (m_process.state() == QProcess::NotRunning && m_running_item != nullptr) {
		m_running_item->SetRunning(false);
	}

	Update();
}

void MainDialogPrivate::Update() {
	const auto* item = m_ui->widget->GetSelectedItem();

	bool run_enabled = (m_process.state() == QProcess::NotRunning && item != nullptr);

	if (run_enabled) {
		const auto& info = item->GetInfo();
		auto        dir  = info.basedir;
		run_enabled      = !dir.isEmpty() && (QDir(dir).exists() || GameContent::IsArchive(dir));
	}

	m_ui->widget->SetRunEnabled(run_enabled);
	const bool gt7_selected = item != nullptr &&
	    item->GetInfo().title_id.compare(QStringLiteral("PPSA01317"), Qt::CaseInsensitive) == 0;
	m_ui->gt7_start_button->setEnabled(run_enabled && gt7_selected);
	m_ui->gt7_start_button->setText(m_process.state() == QProcess::Starting ? tr("Starting...") :
	    m_process.state() == QProcess::Running ? tr("Running...") : tr("Start game"));
	m_ui->gt7_dlss_checkbox->setEnabled(m_process.state() == QProcess::NotRunning);
	m_ui->settings_game_button->setEnabled(item != nullptr);
	{
		const QSignalBlocker blocker(m_ui->gt7_dlss_checkbox);
		m_ui->gt7_dlss_checkbox->setChecked(m_ui->widget->Gt7DlssEnabled());
	}
	const bool has_logs = !LauncherDiagnostics::LatestRunDirectory().isEmpty();
	m_copy_log_button->setEnabled(has_logs);
	m_export_logs_button->setEnabled(has_logs);
	m_ui->gt7_start_button->setToolTip(
	    gt7_selected ? QString {} : tr("Select Gran Turismo 7 in the game library to start."));
	if (m_process.state() != QProcess::NotRunning) {
		m_lightbar.Stop();
		return;
	}
	if (!qEnvironmentVariableIsSet("KYTY_LAUNCHER_UI_SMOKE")) {
		m_lightbar.SetColor(m_ui->widget->GetGlobalControllerColor());
	}
}

#include "mainDialog.moc"
