#pragma once

#include <QObject>
#include <QWidget>
#include <QWindow>
#include <QTimer>
#include <QString>
#include <cstdint>

namespace rex::emu {

/// Finds the Android Emulator window by title and embeds it into a Qt widget
class WindowGrabber : public QObject {
    Q_OBJECT

public:
    explicit WindowGrabber(QObject* parent = nullptr);
    ~WindowGrabber() override;

    /// Start searching for emulator window and embed into container
    void startGrabbing(QWidget* container, const QString& avdName);

    /// Stop and release the embedded window
    void release();

    bool isEmbedded() const { return embedded_; }

    /// Get the latest captured frame
    QImage currentFrame() const;
    double fps() const { return fps_; }

Q_SIGNALS:
    void windowFound(uint64_t windowId);
    void embedded();
    void frameReady();
    void error(const QString& message);

private slots:
    void searchForWindow();
    void captureFrame();

private:
    static uint64_t findEmulatorWindowId(const QString& avdName);

    QWidget* container_ = nullptr;
    QTimer* search_timer_ = nullptr;
    QTimer* capture_timer_ = nullptr;
    QString avd_name_;
    bool embedded_ = false;
    int search_retries_ = 0;
    uint64_t captured_wid_ = 0;

    mutable QMutex frame_mutex_;
    QImage frame_;
    int frame_count_ = 0;
    double fps_ = 0.0;
    qint64 last_fps_time_ = 0;

    static constexpr int kMaxRetries = 30;
};

} // namespace rex::emu
