#include "qmp_client.h"
#include <QJsonArray>
#include <cstdio>

namespace rex::qemu {

QmpClient::QmpClient(QObject* parent) : QObject(parent) {
    socket_ = new QLocalSocket(this);
    connect(socket_, &QLocalSocket::connected, this, &QmpClient::onConnected);
    connect(socket_, &QLocalSocket::readyRead, this, &QmpClient::onReadyRead);
    connect(socket_, &QLocalSocket::disconnected, this, &QmpClient::onDisconnected);
    connect(socket_, &QLocalSocket::errorOccurred, this, &QmpClient::onError);
}

QmpClient::~QmpClient() {
    disconnect();
}

void QmpClient::connectToSocket(const QString& path) {
    ready_ = false;
    capabilities_sent_ = false;
    buffer_.clear();
    socket_->connectToServer(path);
}

void QmpClient::connectToHost(const QString& host, quint16 port) {
    Q_UNUSED(host);
    Q_UNUSED(port);
    emit error("TCP QMP not yet implemented");
}

void QmpClient::disconnect() {
    if (socket_->state() != QLocalSocket::UnconnectedState) {
        socket_->disconnectFromServer();
    }
    ready_ = false;
}

bool QmpClient::isReady() const {
    return ready_;
}

void QmpClient::execute(const QString& command,
                         const QJsonObject& arguments,
                         QmpCallback callback) {
    QJsonObject req;
    req["execute"] = command;
    if (!arguments.isEmpty()) {
        req["arguments"] = arguments;
    }

    pending_.enqueue({req, callback});

    if (!current_callback_) {
        processNextCommand();
    }
}

void QmpClient::stop(QmpCallback cb) { execute("stop", {}, cb); }
void QmpClient::cont(QmpCallback cb) { execute("cont", {}, cb); }
void QmpClient::systemReset(QmpCallback cb) { execute("system_reset", {}, cb); }
void QmpClient::systemPowerdown(QmpCallback cb) { execute("system_powerdown", {}, cb); }
void QmpClient::quit(QmpCallback cb) { execute("quit", {}, cb); }

void QmpClient::queryStatus(QmpCallback cb) {
    execute("query-status", {}, cb);
}

void QmpClient::snapshotSave(const QString& name, QmpCallback cb) {
    QJsonObject args;
    args["job-id"] = QString("snap-save-%1").arg(name);
    args["tag"] = name;
    args["vmstate"] = name;
    args["devices"] = QJsonArray();
    execute("snapshot-save", args, cb);
}

void QmpClient::snapshotLoad(const QString& name, QmpCallback cb) {
    QJsonObject args;
    args["job-id"] = QString("snap-load-%1").arg(name);
    args["tag"] = name;
    args["vmstate"] = name;
    args["devices"] = QJsonArray();
    execute("snapshot-load", args, cb);
}

void QmpClient::onConnected() {
    fprintf(stderr, "qmp: connected\n");
}

void QmpClient::onReadyRead() {
    buffer_.append(socket_->readAll());

    while (true) {
        int idx = buffer_.indexOf('\n');
        if (idx < 0) break;

        QByteArray line = buffer_.left(idx).trimmed();
        buffer_.remove(0, idx + 1);

        if (!line.isEmpty()) {
            processLine(line);
        }
    }
}

void QmpClient::onDisconnected() {
    ready_ = false;
    emit disconnected();
}

void QmpClient::onError(QLocalSocket::LocalSocketError err) {
    Q_UNUSED(err);
    emit error(socket_->errorString());
}

void QmpClient::processLine(const QByteArray& line) {
    QJsonParseError parseErr;
    auto doc = QJsonDocument::fromJson(line, &parseErr);
    if (doc.isNull()) {
        emit error(QString("QMP parse error: %1").arg(parseErr.errorString()));
        return;
    }

    QJsonObject obj = doc.object();

    if (obj.contains("QMP")) {
        if (!capabilities_sent_) {
            QJsonObject caps;
            caps["execute"] = QString("qmp_capabilities");
            sendRaw(caps);
            capabilities_sent_ = true;
        }
        return;
    }

    if (obj.contains("return") && !ready_) {
        ready_ = true;
        emit ready();
        processNextCommand();
        return;
    }

    if (obj.contains("event")) {
        emit event(obj["event"].toString(),
                   obj.value("data").toObject());
        return;
    }

    if (current_callback_) {
        bool success = obj.contains("return");
        auto cb = current_callback_;
        current_callback_ = nullptr;
        cb(success, obj);
        processNextCommand();
    }
}

void QmpClient::sendRaw(const QJsonObject& obj) {
    QJsonDocument doc(obj);
    socket_->write(doc.toJson(QJsonDocument::Compact) + "\n");
    socket_->flush();
}

void QmpClient::processNextCommand() {
    if (pending_.isEmpty() || !ready_) return;

    auto cmd = pending_.dequeue();
    current_callback_ = cmd.callback;
    sendRaw(cmd.request);
}

} // namespace rex::qemu
