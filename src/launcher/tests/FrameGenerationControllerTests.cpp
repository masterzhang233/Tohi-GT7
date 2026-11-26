#include "frameGenerationController.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <functional>

namespace {
int failures = 0;
void Check(bool pass, const char* message) {
	if (!pass) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}
bool Wait(const std::function<bool()>& condition, int timeout = 4000) {
	QElapsedTimer elapsed;
	elapsed.start();
	do {
		QCoreApplication::processEvents();
		if (condition()) {
			return true;
		}
		QThread::msleep(10);
	} while (elapsed.elapsed() < timeout);
	return false;
}
bool Write(const QString& path, const QByteArray& bytes) {
	QSaveFile file(path);
	return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
int ReadCount(const QString& path) {
	QFile file(path);
	return file.open(QIODevice::ReadOnly) ? file.readAll().toInt() : 0;
}
}

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	const auto arguments = app.arguments();
	if (arguments.contains(QStringLiteral("--fake-target"))) {
		QTimer::singleShot(150, &app, &QCoreApplication::quit);
		return app.exec();
	}
	const int child = arguments.indexOf(QStringLiteral("--target-pid"));
	if (child >= 0) {
		// Fake capture host: a real child process, no game, graphics, global settings or saves.
		const auto status = arguments.at(arguments.indexOf(QStringLiteral("--status-file")) + 1);
		const auto stop = arguments.at(arguments.indexOf(QStringLiteral("--stop-file")) + 1);
		const auto target = arguments.at(child + 1).toLongLong();
		const auto attempt_path = status + QStringLiteral(".attempts");
		const int attempt = ReadCount(attempt_path) + 1;
		Write(attempt_path, QByteArray::number(attempt));
		Write(status + QStringLiteral(".ready"), "ready");
		QFile scenario_file(status + QStringLiteral(".scenario"));
		const auto scenario = scenario_file.open(QIODevice::ReadOnly) ? scenario_file.readAll() : QByteArray {};
		const auto publish = [&](const QString& state, int error, qint64 captured, qint64 real, qint64 generated) {
			QJsonObject value {{QStringLiteral("target_pid"), target},
			                  {QStringLiteral("state"), state},
			                  {QStringLiteral("message"),
			                   (scenario == "recover" || scenario == "always45")
			                       ? QStringLiteral("MP error %1 / system 0: ").arg(error)
			                       : QStringLiteral("Frame generation stopped (MP error %1). See the component log.").arg(error)},
			                  {QStringLiteral("captured"), captured},
			                  {QStringLiteral("real_presented"), real},
			                  {QStringLiteral("generated_presented"), generated},
			                  {QStringLiteral("captured_fps"), 40},
			                  {QStringLiteral("output_fps"), 80}};
			if (scenario == "missing-count") {
				value.remove(QStringLiteral("captured"));
			}
			Write(status, QJsonDocument(value).toJson());
		};
		if (!scenario.isEmpty()) {
			QTimer::singleShot(20, &app, [&] {
				if ((scenario == "recover" && attempt > 1) || scenario == "after-running") {
					publish(QStringLiteral("running"), 0, 5, 5, 5);
					if (scenario == "after-running") {
						QTimer::singleShot(1100, &app, [&] {
							// Even reset/malformed final counters cannot erase observed activity.
							publish(QStringLiteral("failed"), 45, 0, 0, 0);
							app.exit(11);
						});
					}
				} else {
					publish(scenario == "not-failed" ? QStringLiteral("waiting") : QStringLiteral("failed"),
					        scenario == "other-error" ? 46 : 45, scenario == "after-capture" ? 1 : 0, 0, 0);
					app.exit(11);
				}
			});
		}
		QTimer timer;
		timer.setInterval(20);
		QObject::connect(&timer, &QTimer::timeout, &app, [&] {
			if (QFile::exists(stop)) {
				Write(status + QStringLiteral(".clean-exit"), "yes");
				app.quit();
			}
		});
		timer.start();
		QTimer::singleShot(20000, &app, &QCoreApplication::quit);
		return app.exec();
	}

	QTemporaryDir temp;
	Check(temp.isValid(), "temporary directory");
	QString text;
	bool active = false;
	FrameGenerationController missing(nullptr, temp.filePath(QStringLiteral("missing-host.exe")));
	QObject::connect(&missing, &FrameGenerationController::StatusChanged, &app,
	                 [&](const QString& value, bool enabled) { text = value; active = enabled; });
	missing.Start(app.applicationPid(), temp.path());
	Check(!active && text.contains(QStringLiteral("missing")), "missing host falls back without launching or ending game");

	FrameGenerationController controller(nullptr, app.applicationFilePath());
	QObject::connect(&controller, &FrameGenerationController::StatusChanged, &app,
	                 [&](const QString& value, bool enabled) { text = value; active = enabled; });
	const auto status = temp.filePath(QStringLiteral("framegen-status.json"));
	const auto ready = status + QStringLiteral(".ready");
	const auto clean = status + QStringLiteral(".clean-exit");
	controller.setEnabled(false);
	controller.Start(app.applicationPid(), temp.path());
	Check(!Wait([&] { return QFile::exists(ready); }, 250), "disabled FG never spawns its host");
	controller.setEnabled(true);
	Check(Wait([&] { return QFile::exists(ready); }), "enabling during a game starts host");
	const auto publish = [&](qint64 pid, qint64 generated) {
		QJsonObject state;
		state.insert(QStringLiteral("target_pid"), pid);
		state.insert(QStringLiteral("state"), QStringLiteral("running"));
		state.insert(QStringLiteral("generated_presented"), generated);
		state.insert(QStringLiteral("captured_fps"), 40);
		state.insert(QStringLiteral("output_fps"), 80);
		Check(Write(status, QJsonDocument(state).toJson()), "write host status");
	};
	publish(app.applicationPid() + 1, 20);
	Check(!Wait([&] { return active; }, 1100), "status for a different game cannot claim active FG");
	publish(app.applicationPid(), 0);
	Check(Wait([&] { return text.contains(QStringLiteral("first generated frame")); }),
	      "feature creation alone does not mark FG active");
	Check(!active, "zero generated presents remains inactive");
	publish(app.applicationPid(), 20);
	Check(Wait([&] { return active && text.contains(QStringLiteral("80")); }),
	      "successful generated presents and measured FPS mark FG active");
	QTimer heartbeats;
	heartbeats.setInterval(300);
	QObject::connect(&heartbeats, &QTimer::timeout, &app, [&] { publish(app.applicationPid(), 20); });
	heartbeats.start();
	Check(Wait([&] { return !active && text.contains(QStringLiteral("new generated frames")); }, 5000),
	      "live heartbeats with a stalled cumulative count cannot keep FG active");
	heartbeats.stop();
	publish(app.applicationPid(), 21);
	Check(Wait([&] { return active; }), "new generated presents recover the active status");
	controller.setEnabled(false);
	Check(!active, "disabling clears active state immediately");
	Check(Wait([&] { return QFile::exists(clean); }), "disable requests a clean host exit");
	QFile::remove(ready);
	QFile::remove(clean);
	controller.setEnabled(true);
	Check(Wait([&] { return QFile::exists(ready); }), "host can restart after clean shutdown");
	Check(!active, "previous run's status is removed before restarting");
	controller.Stop();
	Check(Wait([&] { return QFile::exists(clean); }), "game completion cleanly stops host");
	QFile::remove(ready);
	Check(!Wait([&] { return QFile::exists(ready); }, 1000), "Stop does not restart a finished game host");

	const auto scenarioPath = [&](const QString& name, const QByteArray& mode) {
		const auto directory = temp.filePath(name);
		Check(QDir().mkpath(directory), "create retry scenario directory");
		const auto path = QDir(directory).filePath(QStringLiteral("framegen-status.json"));
		Check(Write(path + QStringLiteral(".scenario"), mode), "write fake-host scenario");
		return path;
	};
	{
		const auto path = scenarioPath(QStringLiteral("retry-recovery"), "recover");
		bool recovery_active = false;
		bool waited = false;
		FrameGenerationController retry(nullptr, app.applicationFilePath(), 50);
		QObject::connect(&retry, &FrameGenerationController::StatusChanged, &app,
		                 [&](const QString& value, bool enabled) {
			                 recovery_active = enabled;
			                 waited |= value.contains(QStringLiteral("retry 1/4"));
		                 });
		retry.Start(app.applicationPid(), QFileInfo(path).absolutePath());
		Check(Wait([&] { return recovery_active; }), "zero-frame error 45 retries and recovers on second host");
		Check(waited && ReadCount(path + QStringLiteral(".attempts")) == 2,
		      "one transient failure has exactly one retry with a visible waiting state");
		retry.Stop();
		Check(Wait([&] { return QFile::exists(path + QStringLiteral(".clean-exit")); }),
		      "recovered host still responds to graceful stop");
	}
	for (const bool disable : {false, true}) {
		const auto path = scenarioPath(disable ? QStringLiteral("retry-disable") : QStringLiteral("retry-stop"), "always45");
		bool pending = false;
		FrameGenerationController retry(nullptr, app.applicationFilePath(), 100);
		QObject::connect(&retry, &FrameGenerationController::StatusChanged, &app,
		                 [&](const QString& value, bool) { pending = value.contains(QStringLiteral("retry 1/4")); });
		retry.Start(app.applicationPid(), QFileInfo(path).absolutePath());
		Check(Wait([&] { return pending; }), "startup retry becomes pending");
		if (disable) { retry.setEnabled(false); } else { retry.Stop(); }
		Check(!Wait([&] { return ReadCount(path + QStringLiteral(".attempts")) > 1; }, 350),
		      disable ? "FG off cancels a pending retry" : "game Stop cancels a pending retry");
	}
	for (const auto* mode : {"other-error", "after-capture", "missing-count", "not-failed"}) {
		const auto path = scenarioPath(QString::fromLatin1(mode), mode);
		FrameGenerationController retry(nullptr, app.applicationFilePath(), 50);
		retry.Start(app.applicationPid(), QFileInfo(path).absolutePath());
		Check(Wait([&] { return QFile::exists(path); }), "non-retry scenario publishes status");
		Check(!Wait([&] { return ReadCount(path + QStringLiteral(".attempts")) > 1; }, 250),
		      "other errors, captured frames, missing counts and non-failed status are not retried");
	}
	{
		const auto path = scenarioPath(QStringLiteral("after-running"), "after-running");
		bool was_active = false;
		bool failed = false;
		FrameGenerationController retry(nullptr, app.applicationFilePath(), 50);
		QObject::connect(&retry, &FrameGenerationController::StatusChanged, &app,
		                 [&](const QString& value, bool enabled) {
			                 was_active |= enabled;
			                 failed |= value.contains(QStringLiteral("unavailable"));
		                 });
		retry.Start(app.applicationPid(), QFileInfo(path).absolutePath());
		Check(Wait([&] { return failed; }) && was_active, "simulate error 45 after successful presentation");
		Check(!Wait([&] { return ReadCount(path + QStringLiteral(".attempts")) > 1; }, 250),
		      "an error after running cannot restart FG even if final counts are zero");
	}
	{
		const auto path = scenarioPath(QStringLiteral("bounded-retries"), "always45");
		QString last_text;
		FrameGenerationController retry(nullptr, app.applicationFilePath(), 50);
		QObject::connect(&retry, &FrameGenerationController::StatusChanged, &app,
		                 [&](const QString& value, bool) { last_text = value; });
		retry.Start(app.applicationPid(), QFileInfo(path).absolutePath());
		Check(Wait([&] { return ReadCount(path + QStringLiteral(".attempts")) == 5 && last_text.contains(QStringLiteral("unavailable")); }),
		      "persistent startup error stops after the initial attempt and four retries");
		Check(!Wait([&] { return ReadCount(path + QStringLiteral(".attempts")) > 5; }, 400),
		      "startup retry budget is finite");
	}
	{
		const auto path = scenarioPath(QStringLiteral("target-exit"), "always45");
		QProcess target;
		target.start(app.applicationFilePath(), {QStringLiteral("--fake-target")});
		Check(target.waitForStarted(), "start short-lived fake target");
		FrameGenerationController retry(nullptr, app.applicationFilePath(), 300);
		retry.Start(target.processId(), QFileInfo(path).absolutePath());
		Check(Wait([&] { return target.state() == QProcess::NotRunning; }), "fake target exits by itself");
		Check(!Wait([&] { return ReadCount(path + QStringLiteral(".attempts")) > 1; }, 600),
		      "pending retry does not restart capture after target exit");
	}
	std::printf("Frame generation lifecycle tests: %s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
