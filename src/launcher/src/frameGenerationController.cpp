#include "frameGenerationController.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#endif

namespace {
constexpr int STARTUP_RETRY_FACTORS[] = {1, 2, 4, 6};
constexpr int MAX_STARTUP_RETRIES = 4;
}

FrameGenerationController::FrameGenerationController(QObject* parent, const QString& executable,
                                                     int retry_delay_ms):
    QObject(parent),
    m_executable(executable.isEmpty()
                     ? QDir(QCoreApplication::applicationDirPath()).filePath(
                           QStringLiteral("framegen/TohiFrameGen.exe"))
                     : executable),
    m_retry_delay_ms(qBound(1, retry_delay_ms, 2000)) {
	m_poll.setInterval(750);
	m_stop_timeout.setSingleShot(true);
	m_stop_timeout.setInterval(5000);
	m_startup_retry.setSingleShot(true);
	connect(&m_startup_retry, &QTimer::timeout, this, [this] {
		if (!m_enabled || m_stopping || m_requested_pid <= 0 || m_process.state() != QProcess::NotRunning) {
			return;
		}
		if (!TargetIsAlive()) {
			Stop();
			return;
		}
		Begin();
	});
	connect(&m_poll, &QTimer::timeout, this, &FrameGenerationController::ReadStatus);
	connect(&m_stop_timeout, &QTimer::timeout, this, [this] {
		// QProcess retains the handle of the exact helper we created; never kill by name/PID.
		if (m_stopping && m_process.state() != QProcess::NotRunning) {
			m_process.kill();
		}
	});
	connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
		if (!m_stopping && error == QProcess::FailedToStart) {
			m_poll.stop();
			Report(tr("Frame generation could not start. The game continues without it. (%1)")
			           .arg(m_process.errorString()));
		}
	});
	connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
	        [this](int code, QProcess::ExitStatus exit_status) {
		        m_stop_timeout.stop();
		        if (!m_stopping) {
			        ReadStatus();
		        }
		        m_poll.stop();
		        m_active_pid = 0;
		        if (m_stopping) {
			        m_stopping = false;
			        if (m_enabled && m_requested_pid > 0) {
				        Begin();
			        }
		        } else if (m_enabled && m_requested_pid > 0 && m_retryable_startup_failure &&
		                   !m_seen_frames && code == 11 && exit_status == QProcess::NormalExit &&
		                   m_startup_retries < MAX_STARTUP_RETRIES && TargetIsAlive()) {
			        // Only the host's explicit pre-capture SourceWindowUnresponsive failure.
			        // Never restart a working session, a crash, or another initialization error.
			        const int delay = m_retry_delay_ms * STARTUP_RETRY_FACTORS[m_startup_retries++];
			        m_startup_retry.start(delay);
			        Report(tr("Waiting for the game window to respond · retry %1/%2…")
			                   .arg(m_startup_retries).arg(MAX_STARTUP_RETRIES));
		        } else if (m_last_state != QStringLiteral("failed")) {
			        Report(code == 0 && exit_status == QProcess::NormalExit
			                   ? tr("Frame generation stopped.")
			                   : tr("Frame generation ended unexpectedly. The game continues without it."));
		        }
	        });
#if defined(_WIN32)
	m_process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
		args->flags |= CREATE_NO_WINDOW;
	});
#endif
}

FrameGenerationController::~FrameGenerationController() {
	Stop();
	m_process.disconnect(this);
	if (m_process.state() != QProcess::NotRunning && !m_process.waitForFinished(1500)) {
		m_process.kill();
		m_process.waitForFinished(500);
	}
}

void FrameGenerationController::Report(const QString& text, bool active) {
	if (m_last_text == text && m_active == active) {
		return;
	}
	m_last_text = text;
	m_active = active;
	emit StatusChanged(text, active);
}

void FrameGenerationController::Start(qint64 game_pid, const QString& run_directory) {
	m_startup_retry.stop();
	m_startup_retries = 0;
	m_seen_frames = false;
	m_requested_pid = game_pid > 0 ? game_pid : 0;
	m_requested_directory = run_directory;
	if (m_process.state() != QProcess::NotRunning) {
		StopHost();
	} else if (m_enabled && m_requested_pid > 0) {
		Begin();
	} else {
		Report(m_enabled ? tr("Waiting for the game.") : tr("Frame generation is off."));
	}
}

void FrameGenerationController::Stop() {
	m_requested_pid = 0;
	StopHost();
	Report(m_enabled ? tr("Waiting for the game.") : tr("Frame generation is off."));
}

void FrameGenerationController::setEnabled(bool enabled) {
	if (m_enabled == enabled) {
		return;
	}
	m_enabled = enabled;
	if (!enabled) {
		StopHost();
		Report(tr("Frame generation is off."));
	} else if (m_process.state() == QProcess::NotRunning && m_requested_pid > 0) {
		Begin();
	} else {
		Report(tr("Waiting for the game."));
	}
}

void FrameGenerationController::StopHost() {
	m_startup_retry.stop();
	m_poll.stop();
	if (m_process.state() == QProcess::NotRunning || m_stopping) {
		return;
	}
	m_stopping = true;
	QSaveFile stop(m_stop_path);
	if (stop.open(QIODevice::WriteOnly)) {
		stop.write("stop\n");
		stop.commit();
	}
	m_stop_timeout.start();
}

bool FrameGenerationController::TargetIsAlive() const {
	if (m_requested_pid <= 0) {
		return false;
	}
#if defined(_WIN32)
	const HANDLE target = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(m_requested_pid));
	if (!target) {
		return false;
	}
	const bool alive = WaitForSingleObject(target, 0) == WAIT_TIMEOUT;
	CloseHandle(target);
	return alive;
#else
	return kill(static_cast<pid_t>(m_requested_pid), 0) == 0 || errno == EPERM;
#endif
}

void FrameGenerationController::Begin() {
	if (!QFileInfo(m_executable).isFile()) {
		Report(tr("Frame generation component is missing. The game continues without it."));
		return;
	}
	auto directory = m_requested_directory;
	if (directory.isEmpty()) {
		directory = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
		                .filePath(QStringLiteral("Tohi-GT7/framegen-runs/") +
		                          QUuid::createUuid().toString(QUuid::WithoutBraces));
	}
	if (!QDir().mkpath(directory)) {
		Report(tr("Frame generation could not create its status folder. The game continues without it."));
		return;
	}
	m_status_path = QDir(directory).absoluteFilePath(QStringLiteral("framegen-status.json"));
	m_stop_path = QDir(directory).absoluteFilePath(QStringLiteral("framegen-stop"));
	// Only these two controller-owned files are replaced; never touch game files or saves.
	QFile::remove(m_status_path);
	QFile::remove(m_stop_path);
	m_last_state.clear();
	m_retryable_startup_failure = false;
	m_status_modified = 0;
	m_generated_presented = 0;
	m_last_generated_frame.invalidate();
	m_last_status.start();
	m_stopping = false;
	m_active_pid = m_requested_pid;
	m_process.setProgram(m_executable);
	m_process.setWorkingDirectory(QFileInfo(m_executable).absolutePath());
	m_process.setArguments({QStringLiteral("--target-pid"), QString::number(m_active_pid),
	                        QStringLiteral("--parent-pid"), QString::number(QCoreApplication::applicationPid()),
	                        QStringLiteral("--status-file"), m_status_path,
	                        QStringLiteral("--stop-file"), m_stop_path,
	                        QStringLiteral("--preset"), QStringLiteral("auto")});
	m_process.setProcessChannelMode(QProcess::MergedChannels);
	m_process.setStandardOutputFile(QDir(directory).absoluteFilePath(QStringLiteral("framegen-host.log")),
	                                QIODevice::Append);
	Report(tr("Waiting for the game window and frame generation…"));
	m_process.start();
	m_poll.start();
}

void FrameGenerationController::ReadStatus() {
	if (m_stopping || m_active_pid <= 0) {
		return;
	}
	if (m_last_status.isValid() && m_last_status.elapsed() > 10000 && m_active) {
		Report(tr("Frame generation status is not responding."));
	} else if (m_active && m_last_generated_frame.isValid() && m_last_generated_frame.elapsed() > 3000) {
		Report(tr("Waiting for new generated frames…"));
	}
	const QFileInfo info(m_status_path);
	const qint64 modified = info.lastModified().toMSecsSinceEpoch();
	if (!info.isFile() || info.size() > 65536 || modified == m_status_modified) {
		return;
	}
	QFile file(m_status_path);
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject()) {
		return; // A partially written or invalid status is never evidence of working FG.
	}
	const auto status = document.object();
	if (status.value(QStringLiteral("target_pid")).toInteger() != m_active_pid) {
		return;
	}
	m_status_modified = modified;
	m_last_status.restart();
	m_last_state = status.value(QStringLiteral("state")).toString();
	bool explicit_zero_counts = true;
	for (const auto* name : {"captured", "real_presented", "generated_presented"}) {
		const auto value = status.value(QLatin1String(name));
		// Missing/invalid counts are not evidence that this was a startup-only failure.
		explicit_zero_counts &= value.isDouble() && value.toDouble(-1) == 0;
		m_seen_frames |= value.isDouble() && value.toDouble() > 0;
	}
	static const QRegularExpression unresponsive_error(
	    QStringLiteral("\\bMP error 45(?:\\)| / system [0-9]+:)"));
	m_retryable_startup_failure = m_last_state == QStringLiteral("failed") && explicit_zero_counts &&
	                              !m_seen_frames && unresponsive_error.match(
	                                  status.value(QStringLiteral("message")).toString()).hasMatch();
	if (m_last_state == QStringLiteral("running")) {
		const qint64 generated = status.value(QStringLiteral("generated_presented")).toInteger();
		if (generated <= 0) {
			Report(tr("Waiting for the first generated frame…"));
			return;
		}
		if (generated != m_generated_presented) {
			m_generated_presented = generated;
			m_last_generated_frame.start();
		}
		// A live heartbeat or historical present count is not proof that interpolation
		// is still producing frames. Keep a separate deadline for actual progress.
		if (!m_last_generated_frame.isValid() || m_last_generated_frame.elapsed() > 3000) {
			Report(tr("Waiting for new generated frames…"));
			return;
		}
		const double captured = status.value(QStringLiteral("captured_fps")).toDouble();
		const double output = status.value(QStringLiteral("output_fps")).toDouble();
		if (captured > 0 && output > 0) {
			Report(tr("2× frame generation active · %1 → %2 FPS")
			           .arg(captured, 0, 'f', 0).arg(output, 0, 'f', 0), true);
		} else {
			Report(tr("2× frame generation active"), true);
		}
	} else if (m_last_state == QStringLiteral("failed")) {
		Report(tr("Frame generation unavailable: %1. The game continues without it.")
		           .arg(status.value(QStringLiteral("message")).toString().left(400)));
	} else if (m_last_state == QStringLiteral("paused")) {
		Report(tr("Frame generation paused · Alt+Shift+A to resume"));
	} else if (m_last_state == QStringLiteral("stopped")) {
		Report(tr("Frame generation stopped."));
	} else {
		Report(tr("Waiting for the game window and frame generation…"));
	}
}
