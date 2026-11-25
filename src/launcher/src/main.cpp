#include "mainDialog.h"
#include "launcherLanguage.h"
#include "launcherTheme.h"

#include <QApplication>
#include <QCheckBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QRegularExpression>
#include <cstdio>
#include <QPushButton>
#include <QTimer>
#include <QTranslator>

int main(int argc, char* argv[]) {
	QApplication a(argc, argv);
	const auto arguments = a.arguments();
	const int screenshot_option = arguments.indexOf(QStringLiteral("--ui-screenshot"));
	const bool smoke = screenshot_option >= 0 && screenshot_option + 1 < arguments.size();
	if (smoke) {
		// Preview never changes real settings, starts the emulator, or prompts for game folders.
		qputenv("KYTY_LAUNCHER_UI_SMOKE", "1");
	}
	QString language = LauncherLanguage::ReadStartupPreference();
	QSize preview_size(1120, 760);
	if (smoke) {
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

	MainDialog w;

	if (!smoke) {
		w.emit Start();
	} else {
		w.resize(preview_size);
	}

	w.show();
	if (smoke) {
		const auto path = arguments.at(screenshot_option + 1);
		QTimer::singleShot(600, &w, [&a, &w, path, language] {
			const bool saved = w.grab().save(path);
			QJsonObject state;
			state.insert(QStringLiteral("title"), w.windowTitle());
			state.insert(QStringLiteral("language"), language);
			const auto* summary = w.findChild<QLabel*>(QStringLiteral("gt7_feature_summary"));
			state.insert(QStringLiteral("graphics_summary"), summary ? summary->text() : QString());
			state.insert(QStringLiteral("width"), w.width());
			state.insert(QStringLiteral("height"), w.height());
			const auto* checkbox = w.findChild<QCheckBox*>(QStringLiteral("gt7_dlss_checkbox"));
			state.insert(QStringLiteral("dlss_checked"), checkbox != nullptr && checkbox->isChecked());
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
