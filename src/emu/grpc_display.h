#pragma once

#include <QObject>
#include <QImage>
#include <QMutex>
#include <QThread>
#include <QString>
#include <memory>
#include <atomic>

// Forward-declare gRPC types to avoid including heavy headers here
namespace grpc { class Channel; }
namespace android::emulation::control { class EmulatorController; }

namespace rex::emu {

/// gRPC-based display client for Android Emulator
/// Streams framebuffer via EmulatorController::streamScreenshot
class GrpcDisplay : public QObject {
    Q_OBJECT

public:
    explicit GrpcDisplay(QObject* parent = nullptr);
    ~GrpcDisplay() override;

    /// Connect to emulator gRPC endpoint
    void connectToEmulator(const QString& host, uint16_t port);

    /// Disconnect and stop streaming
    void disconnect();

    bool isConnected() const { return connected_.load(); }

    /// Get current frame (thread-safe)
    QImage currentFrame() const;

    int width() const { return width_; }
    int height() const { return height_; }
    double fps() const { return fps_; }

    /// Send keyboard event
    void sendKey(int keyCode, bool down);

    /// Send touch event
    void sendTouch(int x, int y, int id, bool down, int pressure = 128);

    /// Send mouse event
    void sendMouse(int x, int y, int buttons);

Q_SIGNALS:
    void connected();
    void disconnected();
    void error(const QString& message);
    void frameReady();
    void resolutionChanged(int width, int height);

private:
    void streamLoop();

    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<QThread> stream_thread_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> should_stop_{false};

    mutable QMutex mutex_;
    QImage frame_;
    int width_ = 0;
    int height_ = 0;

    // FPS tracking
    std::atomic<int> frame_count_{0};
    std::atomic<double> fps_{0.0};
    qint64 last_fps_time_ = 0;

    QString host_;
    uint16_t port_ = 0;
};

} // namespace rex::emu
