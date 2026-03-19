#include "emulator_process.h"
#include <QDir>
#include <QStandardPaths>
#include <cstdio>

namespace rex::emu {

EmulatorProcess::EmulatorProcess(QObject* parent) : QObject(parent) {
    process_ = new QProcess(this);
    connect(process_, &QProcess::started, this, &EmulatorProcess::onProcessStarted);
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &EmulatorProcess::onProcessFinished);
    connect(process_, &QProcess::errorOccurred, this, &EmulatorProcess::onProcessError);

    adb_check_timer_ = new QTimer(this);
    adb_check_timer_->setInterval(2000);
    connect(adb_check_timer_, &QTimer::timeout, this, &EmulatorProcess::checkAdbReady);
}

EmulatorProcess::~EmulatorProcess() {
    stop();
}

void EmulatorProcess::start(const Config& config) {
    if (state_ != State::Stopped) {
        emit error("Emulator already running");
        return;
    }

    config_ = config;
    QString binary = findEmulatorBinary(config_.sdk_root);
    if (binary.isEmpty()) {
        emit error("Android emulator not found. Install Android SDK.");
        return;
    }

    QStringList args;
    args << "-avd" << config_.avd_name;
    if (config_.no_window) args << "-no-window";
    if (config_.no_audio) args << "-no-audio";
    if (config_.no_snapshot) args << "-no-snapshot";
    args << "-gpu" << config_.gpu_mode;
    args << "-grpc" << QString::number(config_.grpc_port);
    args << "-ports" << QString("%1,%2").arg(config_.console_port).arg(config_.adb_port);
    args << config_.extra_args;

    setState(State::Starting);
    fprintf(stderr, "emu: starting %s -avd %s\n",
            binary.toUtf8().constData(), config_.avd_name.toUtf8().constData());

    process_->setProcessChannelMode(QProcess::ForwardedChannels);
    process_->start(binary, args);
}

void EmulatorProcess::stop() {
    adb_check_timer_->stop();
    if (process_->state() != QProcess::NotRunning) {
        // Graceful shutdown via ADB
        QProcess::execute("adb", {"-s", QString("emulator-%1").arg(config_.console_port),
                                  "emu", "kill"});
        if (!process_->waitForFinished(5000)) {
            process_->kill();
        }
    }
    setState(State::Stopped);
}

QString EmulatorProcess::findEmulatorBinary(const QString& sdk_root) {
    // Try explicit SDK root first
    QStringList roots;
    if (!sdk_root.isEmpty()) roots << sdk_root;
    roots << qEnvironmentVariable("ANDROID_SDK_ROOT");
    roots << qEnvironmentVariable("ANDROID_HOME");
    roots << QDir::homePath() + "/Library/Android/sdk";   // macOS default
    roots << QDir::homePath() + "/Android/Sdk";           // Linux default
    roots << "C:/Users/" + qEnvironmentVariable("USERNAME") + "/AppData/Local/Android/Sdk"; // Windows

    for (const auto& root : roots) {
        if (root.isEmpty()) continue;
        QString path = root + "/emulator/emulator";
#ifdef Q_OS_WIN
        path += ".exe";
#endif
        if (QFile::exists(path)) return path;
    }

    // Try PATH
    return QStandardPaths::findExecutable("emulator");
}

QString EmulatorProcess::findOrCreateAvd(const QString& sdk_root) {
    // Check existing AVDs
    QProcess proc;
    QString emulator = findEmulatorBinary(sdk_root);
    if (emulator.isEmpty()) return {};

    proc.start(emulator, {"-list-avds"});
    proc.waitForFinished(5000);
    QString output = proc.readAllStandardOutput().trimmed();
    if (!output.isEmpty()) {
        // Return first AVD
        return output.split('\n').first().trimmed();
    }

    // No AVD found — try to create one
    QString avdmanager;
    QStringList roots;
    if (!sdk_root.isEmpty()) roots << sdk_root;
    roots << qEnvironmentVariable("ANDROID_SDK_ROOT");
    roots << QDir::homePath() + "/Library/Android/sdk";
    roots << QDir::homePath() + "/Android/Sdk";

    for (const auto& root : roots) {
        QString path = root + "/cmdline-tools/latest/bin/avdmanager";
        if (QFile::exists(path)) { avdmanager = path; break; }
    }

    if (!avdmanager.isEmpty()) {
        QProcess create;
        create.start(avdmanager, {"create", "avd", "-n", "RexPlayer", "-k",
                     "system-images;android-36.1;google_apis_playstore;arm64-v8a",
                     "-d", "pixel", "--force"});
        create.write("no\n");
        create.waitForFinished(30000);
        fprintf(stderr, "emu: created AVD 'RexPlayer'\n");
        return "RexPlayer";
    }

    return {};
}

void EmulatorProcess::onProcessStarted() {
    fprintf(stderr, "emu: process started (PID %lld)\n", process_->processId());
    adb_check_retries_ = 0;
    adb_check_timer_->start();
}

void EmulatorProcess::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    adb_check_timer_->stop();
    fprintf(stderr, "emu: exited (code=%d)\n", exitCode);
    setState(State::Stopped);
    emit stopped();
    if (status == QProcess::CrashExit) {
        emit error("Emulator crashed");
    }
}

void EmulatorProcess::onProcessError(QProcess::ProcessError err) {
    QString msg;
    switch (err) {
        case QProcess::FailedToStart: msg = "Emulator binary not found"; break;
        case QProcess::Crashed: msg = "Emulator crashed"; break;
        default: msg = "Emulator error"; break;
    }
    setState(State::Error);
    emit error(msg);
}

void EmulatorProcess::checkAdbReady() {
    adb_check_retries_++;
    if (adb_check_retries_ > kMaxAdbRetries) {
        adb_check_timer_->stop();
        emit error("Emulator boot timeout");
        return;
    }

    // Check if boot completed via ADB
    QProcess adb;
    adb.start("adb", {"-s", QString("emulator-%1").arg(config_.console_port),
                       "shell", "getprop", "sys.boot_completed"});
    adb.waitForFinished(3000);
    QString result = adb.readAllStandardOutput().trimmed();
    if (result == "1") {
        adb_check_timer_->stop();
        fprintf(stderr, "emu: boot completed (took ~%ds)\n", adb_check_retries_ * 2);
        setState(State::Running);
        emit started();
    }
}

void EmulatorProcess::setState(State s) {
    if (state_ != s) {
        state_ = s;
        emit stateChanged(s);
    }
}

} // namespace rex::emu
