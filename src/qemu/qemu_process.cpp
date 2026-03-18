#include "qemu_process.h"
#include <QFile>
#include <cstdio>

namespace rex::qemu {

QemuProcess::QemuProcess(QObject* parent) : QObject(parent) {
    process_ = std::make_unique<QProcess>(this);
    qmp_ = std::make_unique<QmpClient>(this);

    connect(process_.get(), &QProcess::started,
            this, &QemuProcess::onProcessStarted);
    connect(process_.get(), QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &QemuProcess::onProcessFinished);
    connect(process_.get(), &QProcess::errorOccurred,
            this, &QemuProcess::onProcessError);

    connect(qmp_.get(), &QmpClient::ready, this, &QemuProcess::onQmpReady);
    connect(qmp_.get(), &QmpClient::event, this, &QemuProcess::onQmpEvent);
    connect(qmp_.get(), &QmpClient::disconnected, this, [this]() {
        if (state_ == State::Running || state_ == State::Paused) {
            emit error("QMP connection lost");
        }
    });

    heartbeat_timer_ = new QTimer(this);
    connect(heartbeat_timer_, &QTimer::timeout, this, &QemuProcess::onHeartbeat);

    qmp_connect_timer_ = new QTimer(this);
    qmp_connect_timer_->setSingleShot(true);
    connect(qmp_connect_timer_, &QTimer::timeout, this, [this]() {
        if (qmp_connect_retries_ < kMaxQmpRetries && state_ == State::Starting) {
            qmp_connect_retries_++;
            qmp_->connectToSocket(config_.qmp_socket_path);
            qmp_connect_timer_->start(kQmpRetryIntervalMs);
        } else if (state_ == State::Starting) {
            emit error("QMP connection failed after retries");
            kill();
        }
    });
}

QemuProcess::~QemuProcess() {
    kill();
}

void QemuProcess::start(const QemuConfig& config) {
    if (state_ != State::Stopped) {
        emit error("VM already running");
        return;
    }

    config_ = config;
    if (config_.spice_socket_path.isEmpty() || config_.qmp_socket_path.isEmpty()) {
        config_.generateSocketPaths("");
    }

    auto args = config_.toCommandLine();
    if (args.isEmpty()) {
        emit error("Empty QEMU command line");
        return;
    }

    QString binary = args.takeFirst();

    setState(State::Starting);
    fprintf(stderr, "qemu: starting %s\n", binary.toUtf8().constData());

    process_->setProcessChannelMode(QProcess::ForwardedChannels);
    process_->start(binary, args);
}

void QemuProcess::pause() {
    if (state_ != State::Running) return;
    qmp_->stop([this](bool ok, const QJsonObject&) {
        if (ok) setState(State::Paused);
    });
}

void QemuProcess::resume() {
    if (state_ != State::Paused) return;
    qmp_->cont([this](bool ok, const QJsonObject&) {
        if (ok) setState(State::Running);
    });
}

void QemuProcess::reset() {
    qmp_->systemReset();
}

void QemuProcess::poweroff() {
    qmp_->systemPowerdown();
}

void QemuProcess::kill() {
    heartbeat_timer_->stop();
    qmp_connect_timer_->stop();

    if (qmp_->isReady()) {
        qmp_->quit([this](bool, const QJsonObject&) {
            QTimer::singleShot(kKillTimeoutMs, this, [this]() {
                if (process_->state() != QProcess::NotRunning) {
                    fprintf(stderr, "qemu: force killing\n");
                    process_->kill();
                }
            });
        });
    } else if (process_->state() != QProcess::NotRunning) {
        process_->kill();
    }

    qmp_->disconnect();
}

void QemuProcess::snapshotSave(const QString& name) {
    qmp_->snapshotSave(name, [this, name](bool ok, const QJsonObject&) {
        if (!ok) emit error(QString("Snapshot save failed: %1").arg(name));
    });
}

void QemuProcess::snapshotLoad(const QString& name) {
    qmp_->snapshotLoad(name, [this, name](bool ok, const QJsonObject&) {
        if (!ok) emit error(QString("Snapshot load failed: %1").arg(name));
    });
}

void QemuProcess::onProcessStarted() {
    fprintf(stderr, "qemu: process started (PID %lld)\n", process_->processId());
    qmp_connect_retries_ = 0;
    qmp_connect_timer_->start(kQmpRetryIntervalMs);
}

void QemuProcess::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    heartbeat_timer_->stop();
    qmp_connect_timer_->stop();
    fprintf(stderr, "qemu: process exited (code=%d, status=%d)\n", exitCode, (int)status);

    cleanupSockets();
    setState(State::Stopped);
    emit stopped();

    if (status == QProcess::CrashExit) {
        emit error(QString("QEMU crashed (exit code %1)").arg(exitCode));
    }
}

void QemuProcess::onProcessError(QProcess::ProcessError err) {
    QString msg;
    switch (err) {
        case QProcess::FailedToStart: msg = "QEMU binary not found or not executable"; break;
        case QProcess::Crashed:       msg = "QEMU crashed"; break;
        case QProcess::Timedout:      msg = "QEMU timed out"; break;
        default:                      msg = "QEMU process error"; break;
    }
    setState(State::Error);
    emit error(msg);
}

void QemuProcess::onQmpReady() {
    qmp_connect_timer_->stop();
    fprintf(stderr, "qemu: QMP ready\n");
    setState(State::Running);
    heartbeat_timer_->start(kHeartbeatIntervalMs);
    emit qmpReady();
    emit started();
}

void QemuProcess::onQmpEvent(const QString& name, const QJsonObject& data) {
    Q_UNUSED(data);
    if (name == "STOP") {
        setState(State::Paused);
    } else if (name == "RESUME") {
        setState(State::Running);
    } else if (name == "SHUTDOWN" || name == "RESET") {
        fprintf(stderr, "qemu: guest %s\n", name.toUtf8().constData());
    }
}

void QemuProcess::onHeartbeat() {
    if (!qmp_->isReady()) return;
    qmp_->queryStatus([this](bool ok, const QJsonObject&) {
        if (!ok && state_ == State::Running) {
            emit error("QMP heartbeat failed — QEMU may be hung");
        }
    });
}

void QemuProcess::setState(State s) {
    if (state_ != s) {
        state_ = s;
        emit stateChanged(s);
    }
}

void QemuProcess::cleanupSockets() {
#ifndef Q_OS_WIN
    QFile::remove(config_.spice_socket_path);
    QFile::remove(config_.qmp_socket_path);
#endif
}

} // namespace rex::qemu
