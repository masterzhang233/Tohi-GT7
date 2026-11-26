#include "mainDialog.h"
#include "configuration.h"
#include "configurationEditDialog.h"
#include "inputMappingDialog.h"
#include "wheelMappingDialog.h"
#include "launcherLanguage.h"
#include "launcherTheme.h"

#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QRegularExpression>
#include <cstdio>
#include <QPushButton>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QTranslator>
#include <QTreeWidget>
#include <memory>

int main(int argc, char* argv[]) {
	QApplication a(argc, argv);
	const QIcon launcher_icon(QStringLiteral(":/icons/tohi-gt7.ico"));
	if (!launcher_icon.isNull()) {
		a.setWindowIcon(launcher_icon);
	}
	const auto arguments = a.arguments();
	const int screenshot_option = arguments.indexOf(QStringLiteral("--ui-screenshot"));
	const bool smoke = screenshot_option >= 0 && screenshot_option + 1 < arguments.size();
	if (smoke) {
		// Preview never changes real settings, starts the emulator, or prompts for game folders.
		qputenv("KYTY_LAUNCHER_UI_SMOKE", "1");
	}
	QString language = LauncherLanguage::ReadStartupPreference();
	QSize preview_size(1120, 760);
	QString preview_page = QStringLiteral("games");
	if (smoke) {
		const int page_option = arguments.indexOf(QStringLiteral("--ui-page"));
		if (page_option >= 0 && page_option + 1 < arguments.size()) {
			preview_page = arguments.at(page_option + 1);
			if (!QStringList {QStringLiteral("games"), QStringLiteral("graphics"),
			                  QStringLiteral("settings"), QStringLiteral("help")}.contains(preview_page)) {
				std::fprintf(stderr, "UI preview page must be games, graphics, settings or help.\n");
				return 2;
			}
		}
		const int language_option = arguments.indexOf(QStringLiteral("--ui-language"));
		if (language_option >= 0 && language_option + 1 < arguments.size()) {
			const auto value = arguments.at(language_option + 1);
			if (value != QStringLiteral("en") && value != QStringLiteral("zh_CN")) {
				std::fprintf(stderr, "UI preview language must be en or zh_CN.\n");
				return 2;
			}
			language = value;
		}
		const int size_option = arguments.indexOf(QStringLiteral("--ui-size"));
		if (size_option >= 0 && size_option + 1 < arguments.size()) {
			const auto match = QRegularExpression(QStringLiteral("^([0-9]+)x([0-9]+)$"))
			                       .match(arguments.at(size_option + 1));
			const int width = match.captured(1).toInt(), height = match.captured(2).toInt();
			if (!match.hasMatch() || width < 900 || width > 1920 || height < 660 || height > 1200) {
				std::fprintf(stderr, "UI preview size must be 900x660 through 1920x1200.\n");
				return 2;
			}
			preview_size = QSize(width, height);
		}
	}
	QTranslator  launcher_translator;
	QTranslator  qt_translator;
	(void)LauncherLanguage::Install(a, launcher_translator, qt_translator,
	                                language);
	LauncherTheme::Initialize(a);
	const int dialog_option = arguments.indexOf(QStringLiteral("--ui-dialog"));
	if (smoke && dialog_option >= 0 && dialog_option + 1 < arguments.size()) {
		// Secondary-dialog previews use in-memory settings and never accept/save them.
		const QString kind = arguments.at(dialog_option + 1);
		Configuration preview_configuration;
		preview_configuration.name = QStringLiteral("Gran Turismo 7");
		preview_configuration.title_id = QStringLiteral("PPSA01317");
		preview_configuration.screen_resolution = Configuration::Resolution::R3840X2160;
		preview_configuration.printf_direction = Configuration::LogDirection::Silent;
		std::unique_ptr<QDialog> dialog;
		if (kind == QStringLiteral("global") || kind == QStringLiteral("configuration")) {
			auto settings = std::make_unique<ConfigurationEditDialog>(preview_configuration);
			if (kind == QStringLiteral("global")) {
				settings->SetGlobalSettings({});
			}
			dialog = std::move(settings);
		} else if (kind == QStringLiteral("input")) {
			dialog = std::make_unique<InputMappingDialog>(QStringList {});
		} else if (kind == QStringLiteral("wheel")) {
			dialog = std::make_unique<WheelMappingDialog>(QString {}, false);
		} else {
			std::fprintf(stderr, "UI preview dialog must be global, configuration, input or wheel.\n");
			return 2;
		}
		dialog->setAttribute(Qt::WA_DontShowOnScreen);
		dialog->resize(preview_size);
		dialog->show();
		const QString path = arguments.at(screenshot_option + 1);
		QTimer::singleShot(600, dialog.get(), [&a, &dialog, path, kind, language] {
			const bool saved = dialog->grab().save(path);
			QJsonObject state;
			state.insert(QStringLiteral("dialog"), kind);
			state.insert(QStringLiteral("language"), language);
			state.insert(QStringLiteral("width"), dialog->width());
			state.insert(QStringLiteral("height"), dialog->height());
			state.insert(QStringLiteral("font_family"), dialog->font().family());
			state.insert(QStringLiteral("font_point_size"), dialog->font().pointSizeF());
			QFile metadata(path + QStringLiteral(".json"));
			if (metadata.open(QIODevice::WriteOnly)) {
				metadata.write(QJsonDocument(state).toJson());
			}
			a.exit(saved ? 0 : 1);
		});
		return QApplication::exec();
	}

	MainDialog w;

	if (!smoke) {
		w.emit Start();
	} else {
		w.setAttribute(Qt::WA_DontShowOnScreen);
		w.resize(preview_size);
		if (auto* navigation = w.findChild<QPushButton*>(QStringLiteral("nav_") + preview_page)) {
			navigation->click();
		}
		if (arguments.contains(QStringLiteral("--ui-disable-external-fg"))) {
			w.findChild<QCheckBox*>(QStringLiteral("external_fg_checkbox"))->setChecked(false);
		}
	}

	w.show();
	if (smoke) {
		const auto path = arguments.at(screenshot_option + 1);
		QTimer::singleShot(600, &w, [&a, &w, path, language, preview_page] {
			const bool saved = w.grab().save(path);
			QJsonObject state;
			state.insert(QStringLiteral("title"), w.windowTitle());
			state.insert(QStringLiteral("language"), language);
			state.insert(QStringLiteral("page"), preview_page);
			state.insert(QStringLiteral("platform_style"), a.property("nativeLauncherStyle").toString());
			state.insert(QStringLiteral("native_title_bar"), !w.windowFlags().testFlag(Qt::FramelessWindowHint));
			const auto application_icon = a.windowIcon();
			const auto window_icon = w.windowIcon();
			state.insert(QStringLiteral("application_icon_null"), application_icon.isNull());
			state.insert(QStringLiteral("window_icon_null"), window_icon.isNull());
			const auto icon_sizes = [](const QIcon& icon) {
				QJsonArray sizes;
				for (const auto& size : icon.availableSizes()) {
					sizes.append(QJsonArray {size.width(), size.height()});
				}
				return sizes;
			};
			state.insert(QStringLiteral("application_icon_sizes"), icon_sizes(application_icon));
			state.insert(QStringLiteral("window_icon_sizes"), icon_sizes(window_icon));
			state.insert(QStringLiteral("width"), w.width());
			state.insert(QStringLiteral("height"), w.height());
			// Preview-only geometry makes clipping at the supported minimum size reviewable.
			const auto widget_geometry = [&w](const QWidget* widget) {
				QJsonObject geometry;
				geometry.insert(QStringLiteral("exists"), widget != nullptr);
				if (widget == nullptr) {
					return geometry;
				}
				const QRect bounds(widget->mapTo(&w, QPoint(0, 0)), widget->size());
				QRect visible_bounds = bounds.intersected(w.rect());
				for (const QWidget* parent = widget->parentWidget(); parent != nullptr && parent != &w;
				     parent = parent->parentWidget()) {
					visible_bounds = visible_bounds.intersected(
					    QRect(parent->mapTo(&w, QPoint(0, 0)), parent->size()));
				}
				geometry.insert(QStringLiteral("x"), bounds.x());
				geometry.insert(QStringLiteral("y"), bounds.y());
				geometry.insert(QStringLiteral("width"), bounds.width());
				geometry.insert(QStringLiteral("height"), bounds.height());
				geometry.insert(QStringLiteral("visible"), widget->isVisibleTo(&w));
				geometry.insert(QStringLiteral("within_ancestor_bounds"), visible_bounds == bounds);
				return geometry;
			};
			state.insert(QStringLiteral("start_button_geometry"),
			             widget_geometry(w.findChild<QPushButton*>(QStringLiteral("gt7_start_button"))));
			const auto* game_list = w.findChild<QTreeWidget*>(QStringLiteral("cfgs_list"));
			state.insert(QStringLiteral("game_list_geometry"), widget_geometry(game_list));
			if (game_list != nullptr) {
				state.insert(QStringLiteral("game_list_viewport_geometry"),
				             widget_geometry(game_list->viewport()));
				const auto* horizontal_scrollbar = game_list->horizontalScrollBar();
				auto scrollbar = widget_geometry(horizontal_scrollbar);
				scrollbar.insert(QStringLiteral("minimum"), horizontal_scrollbar->minimum());
				scrollbar.insert(QStringLiteral("maximum"), horizontal_scrollbar->maximum());
				state.insert(QStringLiteral("game_list_horizontal_scrollbar"), scrollbar);
			}
			const auto* checkbox = w.findChild<QCheckBox*>(QStringLiteral("gt7_dlss_checkbox"));
			state.insert(QStringLiteral("dlss_checked"), checkbox != nullptr && checkbox->isChecked());
			state.insert(QStringLiteral("dlss_label"), checkbox ? checkbox->text() : QString());
			const auto* external_fg = w.findChild<QCheckBox*>(QStringLiteral("external_fg_checkbox"));
			state.insert(QStringLiteral("external_fg_checked"), external_fg != nullptr && external_fg->isChecked());
			state.insert(QStringLiteral("external_fg_label"), external_fg ? external_fg->text() : QString());
			state.insert(QStringLiteral("external_fg_geometry"), widget_geometry(external_fg));
			const auto* fg_status = w.findChild<QLabel*>(QStringLiteral("game_fg_status"));
			state.insert(QStringLiteral("game_fg_status"), fg_status ? fg_status->text() : QString());
			state.insert(QStringLiteral("game_fg_status_geometry"), widget_geometry(fg_status));
			state.insert(QStringLiteral("frame_generation_active"),
			             fg_status != nullptr && fg_status->property("frameGenerationActive").toBool());
			state.insert(QStringLiteral("vr_entry_present"),
			             w.findChild<QPushButton*>(QStringLiteral("gt7_start_vr_button")) != nullptr);
			QFile metadata(path + QStringLiteral(".json"));
			if (metadata.open(QIODevice::WriteOnly)) {
				metadata.write(QJsonDocument(state).toJson());
			}
			a.exit(saved ? 0 : 1);
		});
	}

	return QApplication::exec();
}
