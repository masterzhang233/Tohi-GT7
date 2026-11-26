#ifndef FRAME_GENERATION_CONTROLLER_H
#define FRAME_GENERATION_CONTROLLER_H

#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>

// Owns only our capture host. The game process and any separately running Magpie are never
// terminated by this controller. The optional executable path and retry delay support
// isolated lifecycle tests; production waits 2/4/8/12 seconds, at most four retries.
class FrameGenerationController final: public QObject {
	Q_OBJECT
public:
	explicit FrameGenerationController(QObject* parent = nullptr, const QString& executable = {},
	                                   int retry_delay_ms = 2000);
	~FrameGenerationController() override;

	void Start(qint64 game_pid, const QString& run_directory);
	void Stop();
	[[nodiscard]] bool isEnabled() const { return m_enabled; }
	void setEnabled(bool enabled);

signals:
	void StatusChanged(const QString& text, bool active);

private:
	void Begin();
	void StopHost();
	void ReadStatus();
	void Report(const QString& text, bool active = false);
	bool TargetIsAlive() const;

	QProcess m_process;
	QTimer m_poll;
	QTimer m_stop_timeout;
	QTimer m_startup_retry;
	QElapsedTimer m_last_status;
	QElapsedTimer m_last_generated_frame;
	QString m_executable;
	QString m_requested_directory;
	QString m_status_path;
	QString m_stop_path;
	QString m_last_text;
	QString m_last_state;
	qint64 m_requested_pid = 0;
	qint64 m_active_pid = 0;
	qint64 m_status_modified = 0;
	qint64 m_generated_presented = 0;
	int m_retry_delay_ms = 2000;
	int m_startup_retries = 0;
	bool m_seen_frames = false;
	bool m_retryable_startup_failure = false;
	bool m_enabled = true;
	bool m_stopping = false;
	bool m_active = false;
};

#endif
