#pragma once

#include "input_handler.h"
#include "../gpu/display.h"

#include <QImage>
#include <QWidget>
#include <QMutex>
#include <QTimer>
#include <QElapsedTimer>
#include <QPointF>

#include <atomic>
#include <memory>

namespace rex::gui {

/// DisplayWidget renders the guest VM framebuffer into a Qt widget.
///
/// It receives framebuffer data from rex::gpu::Display via the present
/// callback, converts it to a QImage, and paints it with proper aspect
/// ratio letterboxing. Mouse and keyboard events are translated to
/// guest touch/key coordinates via InputHandler.
class DisplayWidget : public QWidget {
    Q_OBJECT

public:
    explicit DisplayWidget(QWidget* parent = nullptr);
    ~DisplayWidget() override;

    /// Connect this widget to a GPU Display backend.
    /// The display's present callback will be wired to trigger repaints.
    void attachDisplay(rex::gpu::Display* display);

    /// Detach from the current display
    void detachDisplay();

    /// Get the input handler for external configuration
    InputHandler& inputHandler() { return input_handler_; }
    const InputHandler& inputHandler() const { return input_handler_; }

    /// Current frames per second (rolling average)
    double fps() const { return fps_.load(std::memory_order_relaxed); }

    /// Set guest resolution (forwarded to input handler)
    void setGuestResolution(uint32_t width, uint32_t height);

    /// Set display rotation (0, 90, 180, 270 degrees)
    void setRotation(int degrees);

    /// Inject a key event into the guest (Linux input key code)
    void injectKey(uint16_t keycode, bool pressed);

    /// Get the viewport rectangle (the area where the guest is rendered)
    QRect viewport() const;

signals:
    /// Emitted when the guest framebuffer is updated (for status bar FPS)
    void framePresented();

    /// Emitted when mouse/touch events generate input for the guest
    void touchInput(const TouchContact& contact);

    /// Emitted when a key event generates input for the guest
    void keyInput(uint16_t linux_keycode, bool pressed);

protected:
    // Qt event overrides
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;

private slots:
    /// Called when new framebuffer data arrives (via queued connection)
    void onFrameReady();

    /// Update FPS counter
    void updateFps();

private:
    void recalcViewport();

    // GPU display backend
    rex::gpu::Display* display_ = nullptr;

    // Double-buffered QImage for rendering
    QImage front_image_;
    QMutex image_mutex_;

    // Viewport (letterboxed guest area within widget)
    QRect viewport_;
    uint32_t guest_width_  = 1080;
    uint32_t guest_height_ = 1920;
    uint32_t native_guest_width_ = 1080;
    uint32_t native_guest_height_ = 1920;

    // Input handler
    InputHandler input_handler_;

    // FPS tracking
    QElapsedTimer fps_timer_;
    std::atomic<double> fps_{0.0};
    uint64_t frame_count_ = 0;
    uint64_t last_frame_count_ = 0;
    QTimer* fps_update_timer_ = nullptr;
};

} // namespace rex::gui
