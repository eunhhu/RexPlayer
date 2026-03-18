#pragma once

#include <QObject>
#include <QLocalSocket>
#include <QJsonObject>
#include <QJsonDocument>
#include <QString>
#include <QQueue>
#include <functional>
#include <memory>

namespace rex::qemu {

using QmpCallback = std::function<void(bool success, const QJsonObject& response)>;

class QmpClient : public QObject {
    Q_OBJECT

public:
    explicit QmpClient(QObject* parent = nullptr);
    ~QmpClient() override;

    void connectToSocket(const QString& path);
    void connectToHost(const QString& host, quint16 port);
    void disconnect();
    bool isReady() const;

    void execute(const QString& command,
                 const QJsonObject& arguments = {},
                 QmpCallback callback = nullptr);

    void stop(QmpCallback cb = nullptr);
    void cont(QmpCallback cb = nullptr);
    void systemReset(QmpCallback cb = nullptr);
    void systemPowerdown(QmpCallback cb = nullptr);
    void quit(QmpCallback cb = nullptr);
    void queryStatus(QmpCallback cb = nullptr);
    void snapshotSave(const QString& name, QmpCallback cb = nullptr);
    void snapshotLoad(const QString& name, QmpCallback cb = nullptr);

signals:
    void ready();
    void disconnected();
    void error(const QString& message);
    void event(const QString& name, const QJsonObject& data);

private slots:
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void onError(QLocalSocket::LocalSocketError err);

private:
    void processLine(const QByteArray& line);
    void sendRaw(const QJsonObject& obj);
    void processNextCommand();

    QLocalSocket* socket_ = nullptr;
    QByteArray buffer_;
    bool capabilities_sent_ = false;
    bool ready_ = false;

    struct PendingCommand {
        QJsonObject request;
        QmpCallback callback;
    };
    QQueue<PendingCommand> pending_;
    QmpCallback current_callback_;
};

} // namespace rex::qemu
