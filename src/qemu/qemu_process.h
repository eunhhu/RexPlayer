#pragma once

#include "qemu_config.h"
#include "qmp_client.h"
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <memory>

namespace rex::qemu {

class QemuProcess : public QObject {
    Q_OBJECT

public:
    enum class State { Stopped, Starting, Running, Paused, Error };
    Q_ENUM(State)

    explicit QemuProcess(QObject* parent = nullptr);
    ~QemuProcess() override;

    void start(const QemuConfig& config);
    void pause();
    void resume();
    void reset();
    void poweroff();
    void kill();
    void snapshotSave(const QString& name);
    void snapshotLoad(const QString& name);

    State state() const { return state_; }
    bool isRunning() const { return state_ == State::Running; }
    QmpClient* qmp() { return qmp_.get(); }
    const QemuConfig& config() const { return config_; }
    QString spiceSocketPath() const { return config_.spice_socket_path; }

signals:
    void stateChanged(State newState);
    void started();
    void stopped();
    void error(const QString& message);
    void qmpReady();

private slots:
    void onProcessStarted();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError err);
    void onQmpReady();
    void onQmpEvent(const QString& name, const QJsonObject& data);
    void onHeartbeat();

private:
    void setState(State s);
    void cleanupSockets();

    QemuConfig config_;
    std::unique_ptr<QProcess> process_;
    std::unique_ptr<QmpClient> qmp_;
    QTimer* heartbeat_timer_ = nullptr;
    QTimer* qmp_connect_timer_ = nullptr;
    State state_ = State::Stopped;
    int qmp_connect_retries_ = 0;
    static constexpr int kMaxQmpRetries = 20;
    static constexpr int kQmpRetryIntervalMs = 250;
    static constexpr int kHeartbeatIntervalMs = 5000;
    static constexpr int kKillTimeoutMs = 5000;
};

} // namespace rex::qemu
