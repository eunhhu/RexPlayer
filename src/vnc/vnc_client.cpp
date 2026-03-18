#include "vnc_client.h"
#include <QDateTime>
#include <QtEndian>
#include <cstdio>
#include <cstring>

namespace rex::vnc {

VncClient::VncClient(QObject* parent) : QObject(parent) {
    socket_ = new QTcpSocket(this);
    connect(socket_, &QTcpSocket::connected, this, &VncClient::onConnected);
    connect(socket_, &QTcpSocket::readyRead, this, &VncClient::onReadyRead);
    connect(socket_, &QTcpSocket::disconnected, this, &VncClient::onDisconnected);
    connect(socket_, &QTcpSocket::errorOccurred, this, &VncClient::onSocketError);

    update_timer_ = new QTimer(this);
    update_timer_->setInterval(16); // ~60fps
    connect(update_timer_, &QTimer::timeout, this, &VncClient::requestUpdate);
}

VncClient::~VncClient() {
    disconnect();
}

void VncClient::connectToHost(const QString& host, quint16 port) {
    state_ = State::WaitingVersion;
    buffer_.clear();
    fprintf(stderr, "vnc: connecting to %s:%d\n", host.toUtf8().constData(), port);
    socket_->connectToHost(host, port);
}

void VncClient::disconnect() {
    update_timer_->stop();
    if (socket_->state() != QAbstractSocket::UnconnectedState) {
        socket_->disconnectFromHost();
    }
    state_ = State::Disconnected;
}

QImage VncClient::currentFrame() const {
    QMutexLocker lock(&mutex_);
    return frame_.copy();
}

void VncClient::sendKeyEvent(bool down, uint32_t key) {
    if (state_ != State::Connected) return;
    // RFB KeyEvent: type(1) + down(1) + padding(2) + key(4) = 8 bytes
    char msg[8] = {};
    msg[0] = 4; // KeyEvent
    msg[1] = down ? 1 : 0;
    qToBigEndian(key, msg + 4);
    socket_->write(msg, 8);
}

void VncClient::sendPointerEvent(int x, int y, uint8_t buttons) {
    if (state_ != State::Connected) return;
    // RFB PointerEvent: type(1) + buttons(1) + x(2) + y(2) = 6 bytes
    char msg[6] = {};
    msg[0] = 5; // PointerEvent
    msg[1] = static_cast<char>(buttons);
    qToBigEndian(static_cast<quint16>(x), msg + 2);
    qToBigEndian(static_cast<quint16>(y), msg + 4);
    socket_->write(msg, 6);
}

void VncClient::onConnected() {
    fprintf(stderr, "vnc: TCP connected\n");
    state_ = State::WaitingVersion;
}

void VncClient::onReadyRead() {
    buffer_.append(socket_->readAll());
    processProtocol();
}

void VncClient::onDisconnected() {
    update_timer_->stop();
    state_ = State::Disconnected;
    emit disconnected();
}

void VncClient::onSocketError(QAbstractSocket::SocketError err) {
    Q_UNUSED(err);
    emit error(socket_->errorString());
}

void VncClient::processProtocol() {
    // Process based on current state
    switch (state_) {
        case State::WaitingVersion:     handleVersion(); break;
        case State::WaitingSecurity:    handleSecurity(); break;
        case State::WaitingSecurityResult: handleSecurityResult(); break;
        case State::WaitingServerInit:  handleServerInit(); break;
        case State::Connected:          handleFramebufferUpdate(); break;
        default: break;
    }
}

void VncClient::handleVersion() {
    // Server sends "RFB 003.008\n" (12 bytes)
    if (buffer_.size() < 12) return;

    QByteArray version = buffer_.left(12);
    buffer_.remove(0, 12);
    fprintf(stderr, "vnc: server version: %s", version.constData());

    // Reply with same version
    socket_->write("RFB 003.008\n", 12);
    state_ = State::WaitingSecurity;
    processProtocol();
}

void VncClient::handleSecurity() {
    // Server sends: num_types(1) + types(num_types bytes)
    if (buffer_.size() < 1) return;
    uint8_t num = static_cast<uint8_t>(buffer_[0]);
    if (buffer_.size() < 1 + num) return;

    buffer_.remove(0, 1 + num);

    // Select "None" security (type 1)
    char sec = 1;
    socket_->write(&sec, 1);
    state_ = State::WaitingSecurityResult;
    processProtocol();
}

void VncClient::handleSecurityResult() {
    // Server sends: result(4) — 0 = OK
    if (buffer_.size() < 4) return;
    uint32_t result = qFromBigEndian<uint32_t>(buffer_.constData());
    buffer_.remove(0, 4);

    if (result != 0) {
        emit error("VNC security handshake failed");
        return;
    }

    fprintf(stderr, "vnc: security OK\n");
    sendClientInit();
    state_ = State::WaitingServerInit;
    processProtocol();
}

void VncClient::handleServerInit() {
    // ServerInit: width(2) + height(2) + pixel_format(16) + name_len(4) + name(name_len)
    if (buffer_.size() < 24) return;

    width_ = qFromBigEndian<quint16>(buffer_.constData());
    height_ = qFromBigEndian<quint16>(buffer_.constData() + 2);
    // pixel format at offset 4, 16 bytes
    uint32_t name_len = qFromBigEndian<uint32_t>(buffer_.constData() + 20);

    if (buffer_.size() < static_cast<int>(24 + name_len)) return;

    QByteArray name = buffer_.mid(24, name_len);
    buffer_.remove(0, 24 + name_len);

    fprintf(stderr, "vnc: server init %dx%d name='%s'\n", width_, height_, name.constData());

    {
        QMutexLocker lock(&mutex_);
        frame_ = QImage(width_, height_, QImage::Format_RGB32);
        frame_.fill(Qt::black);
    }

    emit resolutionChanged(width_, height_);

    // Set our preferred pixel format (32-bit RGBX)
    sendPixelFormat();
    sendSetEncodings();

    state_ = State::Connected;
    emit connected();

    // Start requesting updates
    sendFramebufferUpdateRequest(false);
    update_timer_->start();
}

void VncClient::handleFramebufferUpdate() {
    // Process RFB messages in Connected state
    while (buffer_.size() > 0) {
        uint8_t msg_type = static_cast<uint8_t>(buffer_[0]);

        if (msg_type == 0) {
            // FramebufferUpdate: type(1) + padding(1) + num_rects(2) = 4 bytes header
            if (!in_update_) {
                if (buffer_.size() < 4) return;
                pending_rects_ = qFromBigEndian<quint16>(buffer_.constData() + 2);
                buffer_.remove(0, 4);
                in_update_ = true;
            }

            while (pending_rects_ > 0) {
                // Each rect: x(2) + y(2) + w(2) + h(2) + encoding(4) = 12 bytes header
                if (buffer_.size() < 12) return;

                quint16 rx = qFromBigEndian<quint16>(buffer_.constData());
                quint16 ry = qFromBigEndian<quint16>(buffer_.constData() + 2);
                quint16 rw = qFromBigEndian<quint16>(buffer_.constData() + 4);
                quint16 rh = qFromBigEndian<quint16>(buffer_.constData() + 6);
                int32_t encoding = qFromBigEndian<int32_t>(buffer_.constData() + 8);

                if (encoding == 0) {
                    // Raw encoding: pixel data follows
                    int data_size = rw * rh * bpp_;
                    if (buffer_.size() < 12 + data_size) return;

                    buffer_.remove(0, 12);

                    // Copy pixel data to frame
                    {
                        QMutexLocker lock(&mutex_);
                        if (!frame_.isNull() && rx + rw <= width_ && ry + rh <= height_) {
                            const char* src = buffer_.constData();
                            for (int y = 0; y < rh; y++) {
                                memcpy(frame_.scanLine(ry + y) + rx * bpp_,
                                       src + y * rw * bpp_,
                                       rw * bpp_);
                            }
                        }
                    }
                    buffer_.remove(0, data_size);
                } else {
                    // Unsupported encoding — skip rect header
                    buffer_.remove(0, 12);
                    fprintf(stderr, "vnc: unsupported encoding %d, skipping rect\n", encoding);
                }

                pending_rects_--;
            }

            in_update_ = false;

            // Update FPS
            frame_count_++;
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (last_fps_time_ == 0) {
                last_fps_time_ = now;
            } else if (now - last_fps_time_ >= 1000) {
                fps_ = frame_count_ * 1000.0 / (now - last_fps_time_);
                frame_count_ = 0;
                last_fps_time_ = now;
            }

            emit frameReady();
        } else if (msg_type == 1) {
            // SetColourMapEntries — skip
            if (buffer_.size() < 6) return;
            quint16 num = qFromBigEndian<quint16>(buffer_.constData() + 4);
            int total = 6 + num * 6;
            if (buffer_.size() < total) return;
            buffer_.remove(0, total);
        } else if (msg_type == 2) {
            // Bell — ignore
            buffer_.remove(0, 1);
        } else if (msg_type == 3) {
            // ServerCutText
            if (buffer_.size() < 8) return;
            uint32_t text_len = qFromBigEndian<uint32_t>(buffer_.constData() + 4);
            int total = 8 + static_cast<int>(text_len);
            if (buffer_.size() < total) return;
            buffer_.remove(0, total);
        } else {
            // Unknown message — bail
            fprintf(stderr, "vnc: unknown message type %d\n", msg_type);
            buffer_.clear();
            return;
        }
    }
}

void VncClient::sendClientInit() {
    // shared-flag = 1 (allow other clients)
    char flag = 1;
    socket_->write(&flag, 1);
}

void VncClient::sendPixelFormat() {
    // SetPixelFormat: type(1) + padding(3) + pixel_format(16) = 20 bytes
    char msg[20] = {};
    msg[0] = 0; // SetPixelFormat

    // Pixel format: 32bpp, depth 24, big-endian=0, true-color=1
    // R/G/B: 8 bits each, shifts 16/8/0 (BGRA in memory = RGB32 in Qt)
    msg[4] = 32;  // bits-per-pixel
    msg[5] = 24;  // depth
    msg[6] = 0;   // big-endian
    msg[7] = 1;   // true-color
    // red-max = 255
    qToBigEndian(static_cast<quint16>(255), msg + 8);
    // green-max = 255
    qToBigEndian(static_cast<quint16>(255), msg + 10);
    // blue-max = 255
    qToBigEndian(static_cast<quint16>(255), msg + 12);
    msg[14] = 16; // red-shift
    msg[15] = 8;  // green-shift
    msg[16] = 0;  // blue-shift

    socket_->write(msg, 20);
}

void VncClient::sendSetEncodings() {
    // SetEncodings: type(1) + padding(1) + num(2) + encodings(4 each)
    char msg[8] = {};
    msg[0] = 2; // SetEncodings
    qToBigEndian(static_cast<quint16>(1), msg + 2); // 1 encoding
    qToBigEndian(static_cast<int32_t>(0), msg + 4); // Raw encoding
    socket_->write(msg, 8);
}

void VncClient::sendFramebufferUpdateRequest(bool incremental) {
    if (state_ != State::Connected) return;
    // FramebufferUpdateRequest: type(1) + incremental(1) + x(2) + y(2) + w(2) + h(2) = 10
    char msg[10] = {};
    msg[0] = 3; // FramebufferUpdateRequest
    msg[1] = incremental ? 1 : 0;
    qToBigEndian(static_cast<quint16>(0), msg + 2); // x
    qToBigEndian(static_cast<quint16>(0), msg + 4); // y
    qToBigEndian(static_cast<quint16>(width_), msg + 6);
    qToBigEndian(static_cast<quint16>(height_), msg + 8);
    socket_->write(msg, 10);
}

void VncClient::requestUpdate() {
    sendFramebufferUpdateRequest(true);
}

} // namespace rex::vnc
