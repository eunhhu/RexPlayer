#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QImage>
#include <QMutex>
#include <QTimer>

namespace rex::vnc {

/// Minimal RFB (VNC) client for receiving framebuffer from QEMU
class VncClient : public QObject {
    Q_OBJECT

public:
    explicit VncClient(QObject* parent = nullptr);
    ~VncClient() override;

    void connectToHost(const QString& host, quint16 port);
    void disconnect();

    bool isConnected() const { return state_ != State::Disconnected; }

    QImage currentFrame() const;
    int width() const { return width_; }
    int height() const { return height_; }
    double fps() const { return fps_; }

    /// Send key event
    void sendKeyEvent(bool down, uint32_t key);
    /// Send pointer (mouse) event
    void sendPointerEvent(int x, int y, uint8_t buttons);

Q_SIGNALS:
    void connected();
    void disconnected();
    void error(const QString& message);
    void frameReady();
    void resolutionChanged(int width, int height);

private slots:
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void onSocketError(QAbstractSocket::SocketError err);
    void requestUpdate();

private:
    enum class State {
        Disconnected,
        WaitingVersion,
        WaitingSecurity,
        WaitingSecurityResult,
        WaitingServerInit,
        Connected
    };

    void processProtocol();
    void handleVersion();
    void handleSecurity();
    void handleSecurityResult();
    void handleServerInit();
    void handleFramebufferUpdate();

    // RFB message sending
    void sendClientInit();
    void sendPixelFormat();
    void sendFramebufferUpdateRequest(bool incremental);
    void sendSetEncodings();

    QTcpSocket* socket_ = nullptr;
    QByteArray buffer_;
    State state_ = State::Disconnected;

    // Framebuffer
    mutable QMutex mutex_;
    QImage frame_;
    int width_ = 0;
    int height_ = 0;
    int bpp_ = 4; // bytes per pixel

    // Update timer
    QTimer* update_timer_ = nullptr;

    // FPS tracking
    int frame_count_ = 0;
    double fps_ = 0.0;
    qint64 last_fps_time_ = 0;

    // Parsing state for framebuffer update
    int pending_rects_ = 0;
    bool in_update_ = false;
};

} // namespace rex::vnc
