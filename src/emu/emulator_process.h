#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QString>
#include <cstdint>

namespace rex::emu {

/// Manages the Google Android Emulator as a subprocess
class EmulatorProcess : public QObject {
    Q_OBJECT

public:
    enum class State { Stopped, Starting, Running, Error };
    Q_ENUM(State)

    struct Config {
        QString avd_name;
        QString sdk_root;           // Android SDK root
        uint32_t ram_mb = 4096;
        uint16_t grpc_port = 8554;
        uint16_t adb_port = 5555;
        uint16_t console_port = 5554;
        QString gpu_mode = "swiftshader_indirect";
        bool no_window = true;
        bool no_audio = true;
        bool no_snapshot = true;
        QStringList extra_args;
    };

    explicit EmulatorProcess(QObject* parent = nullptr);
    ~EmulatorProcess() override;

    void start(const Config& config);
    void stop();

    State state() const { return state_; }
    bool isRunning() const { return state_ == State::Running; }
    const Config& config() const { return config_; }
    uint16_t grpcPort() const { return config_.grpc_port; }

    /// Find the emulator binary from SDK
    static QString findEmulatorBinary(const QString& sdk_root = {});

    /// Find or create a default AVD
    static QString findOrCreateAvd(const QString& sdk_root = {});

Q_SIGNALS:
    void stateChanged(State newState);
    void started();
    void stopped();
    void error(const QString& message);

private slots:
    void onProcessStarted();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError err);
    void checkAdbReady();

private:
    void setState(State s);

    Config config_;
    QProcess* process_ = nullptr;
    QTimer* adb_check_timer_ = nullptr;
    State state_ = State::Stopped;
    int adb_check_retries_ = 0;
    static constexpr int kMaxAdbRetries = 60;  // 60 * 2s = 2 min max boot wait
};

} // namespace rex::emu
