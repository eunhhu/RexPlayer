#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTimer>
#include "../../src/qemu/qmp_client.h"

namespace {

class MockQmpServer : public QObject {
    Q_OBJECT
public:
    explicit MockQmpServer(const QString& name, QObject* parent = nullptr)
        : QObject(parent), server_(new QLocalServer(this)) {
        QLocalServer::removeServer(name);
        server_->listen(name);
        connect(server_, &QLocalServer::newConnection, this, [this]() {
            client_ = server_->nextPendingConnection();
            connect(client_, &QLocalSocket::readyRead, this, &MockQmpServer::onReadyRead);
            QJsonObject greeting;
            QJsonObject qmp;
            QJsonObject version;
            version["major"] = 8; version["minor"] = 0; version["micro"] = 0;
            qmp["version"] = version;
            greeting["QMP"] = qmp;
            send(greeting);
        });
    }

    void send(const QJsonObject& obj) {
        if (client_) {
            client_->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n");
            client_->flush();
        }
    }

    QList<QJsonObject> allReceived() const { return all_received_; }

signals:
    void commandReceived(const QJsonObject& cmd);

private slots:
    void onReadyRead() {
        buffer_.append(client_->readAll());
        while (true) {
            int idx = buffer_.indexOf('\n');
            if (idx < 0) break;
            QByteArray line = buffer_.left(idx).trimmed();
            buffer_.remove(0, idx + 1);
            if (!line.isEmpty()) {
                auto doc = QJsonDocument::fromJson(line);
                all_received_.append(doc.object());
                emit commandReceived(doc.object());
            }
        }
    }

private:
    QLocalServer* server_;
    QLocalSocket* client_ = nullptr;
    QByteArray buffer_;
    QList<QJsonObject> all_received_;
};

class QmpClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        int argc = 0;
        if (!QCoreApplication::instance()) {
            app_ = std::make_unique<QCoreApplication>(argc, nullptr);
        }
        socket_name_ = QString("rex-qmp-test-%1").arg(QCoreApplication::applicationPid());
    }

    void processEvents(int ms = 100) {
        QTimer timer;
        timer.setSingleShot(true);
        timer.start(ms);
        while (timer.isActive()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
    }

    std::unique_ptr<QCoreApplication> app_;
    QString socket_name_;
};

TEST_F(QmpClientTest, ConnectsAndNegotiatesCapabilities) {
    MockQmpServer server(socket_name_);
    rex::qemu::QmpClient client;

    QSignalSpy readySpy(&client, &rex::qemu::QmpClient::ready);

    QObject::connect(&server, &MockQmpServer::commandReceived,
                     [&server](const QJsonObject& cmd) {
        if (cmd["execute"].toString() == "qmp_capabilities") {
            QJsonObject resp;
            resp["return"] = QJsonObject();
            server.send(resp);
        }
    });

    client.connectToSocket(socket_name_);
    processEvents(200);

    EXPECT_TRUE(client.isReady());
    EXPECT_EQ(readySpy.count(), 1);
}

TEST_F(QmpClientTest, ExecuteStopCommand) {
    MockQmpServer server(socket_name_);
    rex::qemu::QmpClient client;

    QObject::connect(&server, &MockQmpServer::commandReceived,
                     [&server](const QJsonObject& cmd) {
        QJsonObject resp;
        resp["return"] = QJsonObject();
        server.send(resp);
    });

    client.connectToSocket(socket_name_);
    processEvents(200);

    bool callback_called = false;
    bool callback_success = false;
    client.stop([&](bool success, const QJsonObject&) {
        callback_called = true;
        callback_success = success;
    });
    processEvents(200);

    EXPECT_TRUE(callback_called);
    EXPECT_TRUE(callback_success);

    bool found_stop = false;
    for (const auto& cmd : server.allReceived()) {
        if (cmd["execute"].toString() == "stop") found_stop = true;
    }
    EXPECT_TRUE(found_stop);
}

TEST_F(QmpClientTest, HandlesAsyncEvents) {
    MockQmpServer server(socket_name_);
    rex::qemu::QmpClient client;

    QObject::connect(&server, &MockQmpServer::commandReceived,
                     [&server](const QJsonObject& cmd) {
        QJsonObject resp;
        resp["return"] = QJsonObject();
        server.send(resp);
    });

    client.connectToSocket(socket_name_);
    processEvents(200);

    QSignalSpy eventSpy(&client, &rex::qemu::QmpClient::event);

    QJsonObject evt;
    evt["event"] = QString("STOP");
    evt["data"] = QJsonObject();
    server.send(evt);
    processEvents(200);

    EXPECT_EQ(eventSpy.count(), 1);
    EXPECT_EQ(eventSpy.at(0).at(0).toString(), "STOP");
}

TEST_F(QmpClientTest, QueuedCommands) {
    MockQmpServer server(socket_name_);
    rex::qemu::QmpClient client;

    QObject::connect(&server, &MockQmpServer::commandReceived,
                     [&server](const QJsonObject& cmd) {
        QJsonObject resp;
        resp["return"] = QJsonObject();
        server.send(resp);
    });

    client.connectToSocket(socket_name_);
    processEvents(200);

    int count = 0;
    client.stop([&](bool, const QJsonObject&) { count++; });
    client.cont([&](bool, const QJsonObject&) { count++; });
    client.queryStatus([&](bool, const QJsonObject&) { count++; });
    processEvents(500);

    EXPECT_EQ(count, 3);
}

} // namespace

#include "test_qmp_client.moc"
