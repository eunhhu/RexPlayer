#include "display_widget.h"

#include <QPainter>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QTouchEvent>

#include <algorithm>
#include <cstring>

namespace rex::gui {

DisplayWidget::DisplayWidget(QWidget* parent)
    : QWidget(parent)
{
    // Accept keyboard focus
    setFocusPolicy(Qt::StrongFocus);

    // Track mouse even when no button is pressed (for hover)
    setMouseTracking(true);

    // Accept touch events
    setAttribute(Qt::WA_AcceptTouchEvents, true);

    // Opaque background for performance
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);

    // Initial viewport
    recalcViewport();

    // FPS timer — fires once per second
    fps_timer_.start();
    fps_update_timer_ = new QTimer(this);
    fps_update_timer_->setInterval(1000);
    connect(fps_update_timer_, &QTimer::timeout, this, &DisplayWidget::updateFps);
    fps_update_timer_->start();
}

DisplayWidget::~DisplayWidget() {
    detachDisplay();
}

// ---------------------------------------------------------------------------
// Display attachment
// ---------------------------------------------------------------------------

void DisplayWidget::attachDisplay(rex::gpu::Display* display) {
    detachDisplay();
    display_ = display;

    if (!display_) return;

    // Wire the present callback: when the GPU display swaps buffers,
    // copy the front buffer into our QImage and schedule a repaint.
    // The callback runs on the VMM thread, so we use a queued signal.
    display_->set_present_callback(
        [this](const rex::gpu::FrameBuffer& fb) {
            if (!fb.is_valid()) return;

            // Determine QImage format from PixelFormat
            QImage::Format qfmt = QImage::Format_ARGB32;
            switch (fb.format) {
                case rex::gpu::PixelFormat::BGRA8888:
                    qfmt = QImage::Format_ARGB32;
                    break;
                case rex::gpu::PixelFormat::RGBA8888:
                    qfmt = QImage::Format_RGBA8888;
                    break;
                case rex::gpu::PixelFormat::RGB565:
                    qfmt = QImage::Format_RGB16;
                    break;
                case rex::gpu::PixelFormat::XRGB8888:
                    qfmt = QImage::Format_RGB32;
                    break;
            }

            // Copy framebuffer data into a QImage (deep copy for thread safety)
            QImage new_image(fb.data, fb.width, fb.height,
                             fb.stride, qfmt);

            {
                QMutexLocker lock(&image_mutex_);
                front_image_ = new_image.copy(); // deep copy
            }

            ++frame_count_;

            // Schedule repaint on the GUI thread via queued invocation
            QMetaObject::invokeMethod(this, "onFrameReady",
                                      Qt::QueuedConnection);
        }
    );

    // Update guest resolution from display
    if (display_->is_ready()) {
        setGuestResolution(display_->width(), display_->height());
    }
}

void DisplayWidget::detachDisplay() {
    if (display_) {
        display_->set_present_callback(nullptr);
        display_ = nullptr;
    }
}

void DisplayWidget::setGuestResolution(uint32_t width, uint32_t height) {
    guest_width_  = width;
    guest_height_ = height;
    input_handler_.setGuestResolution(width, height);
    recalcViewport();
    update();
}

QRect DisplayWidget::viewport() const {
    return viewport_;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void DisplayWidget::recalcViewport() {
    if (guest_width_ == 0 || guest_height_ == 0) {
        viewport_ = rect();
        return;
    }

    const int widget_w = width();
    const int widget_h = height();

    const double guest_aspect =
        static_cast<double>(guest_width_) / guest_height_;
    const double widget_aspect =
        static_cast<double>(widget_w) / widget_h;

    int vp_w, vp_h;
    if (widget_aspect > guest_aspect) {
        // Widget is wider than guest — pillarbox (bars on sides)
        vp_h = widget_h;
        vp_w = static_cast<int>(widget_h * guest_aspect);
    } else {
        // Widget is taller than guest — letterbox (bars on top/bottom)
        vp_w = widget_w;
        vp_h = static_cast<int>(widget_w / guest_aspect);
    }

    const int vp_x = (widget_w - vp_w) / 2;
    const int vp_y = (widget_h - vp_h) / 2;

    viewport_ = QRect(vp_x, vp_y, vp_w, vp_h);
    input_handler_.setViewport(vp_x, vp_y, vp_w, vp_h);
}

void DisplayWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);

    // Fill letterbox bars with black
    painter.fillRect(rect(), Qt::black);

    QMutexLocker lock(&image_mutex_);
    if (!front_image_.isNull()) {
        // Scale the framebuffer image into the viewport, maintaining aspect ratio
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(viewport_, front_image_);
    } else {
        // No framebuffer yet — draw a placeholder
        painter.setPen(Qt::gray);
        painter.drawText(viewport_, Qt::AlignCenter,
                         QStringLiteral("No display output"));
    }
}

void DisplayWidget::resizeEvent(QResizeEvent* /*event*/) {
    recalcViewport();
}

// ---------------------------------------------------------------------------
// Frame update
// ---------------------------------------------------------------------------

void DisplayWidget::onFrameReady() {
    update(); // schedule repaint
    emit framePresented();
}

void DisplayWidget::updateFps() {
    const uint64_t current = frame_count_;
    const uint64_t delta = current - last_frame_count_;
    last_frame_count_ = current;

    // Simple calculation: frames in last second
    fps_.store(static_cast<double>(delta), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Mouse events -> touch input
// ---------------------------------------------------------------------------

void DisplayWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        auto contact = input_handler_.handleMousePress(event->position());
        if (contact.active) {
            emit touchInput(contact);
        }
    }
    QWidget::mousePressEvent(event);
}

void DisplayWidget::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) {
        auto contact = input_handler_.handleMouseMove(event->position());
        if (contact.active) {
            emit touchInput(contact);
        }
    }
    QWidget::mouseMoveEvent(event);
}

void DisplayWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        auto contact = input_handler_.handleMouseRelease(event->position());
        emit touchInput(contact);
    }
    QWidget::mouseReleaseEvent(event);
}

// ---------------------------------------------------------------------------
// Keyboard events -> key input
// ---------------------------------------------------------------------------

void DisplayWidget::keyPressEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        return; // Ignore auto-repeat; guest handles its own repeat
    }
    uint16_t code = input_handler_.handleKeyPress(event->key());
    if (code != linux_input::KEY_RESERVED) {
        emit keyInput(code, true);
        event->accept();
    } else {
        QWidget::keyPressEvent(event);
    }
}

void DisplayWidget::keyReleaseEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        return;
    }
    uint16_t code = input_handler_.handleKeyRelease(event->key());
    if (code != linux_input::KEY_RESERVED) {
        emit keyInput(code, false);
        event->accept();
    } else {
        QWidget::keyReleaseEvent(event);
    }
}

// ---------------------------------------------------------------------------
// Touch events (multi-touch from trackpad / touchscreen)
// ---------------------------------------------------------------------------

bool DisplayWidget::event(QEvent* event) {
    if (event->type() == QEvent::TouchBegin ||
        event->type() == QEvent::TouchUpdate ||
        event->type() == QEvent::TouchEnd)
    {
        auto* touch_event = static_cast<QTouchEvent*>(event);
        const auto& points = touch_event->points();

        std::vector<std::pair<int, QPointF>> touch_points;
        touch_points.reserve(points.size());

        bool any_released = false;
        for (const auto& pt : points) {
            touch_points.emplace_back(pt.id(), pt.position());
            if (pt.state() == QEventPoint::Released) {
                any_released = true;
            }
        }

        auto contacts = input_handler_.handleTouchEvent(
            touch_points,
            event->type() == QEvent::TouchEnd
        );

        for (const auto& c : contacts) {
            emit touchInput(c);
        }

        event->accept();
        return true;
    }

    return QWidget::event(event);
}

void DisplayWidget::setRotation(int degrees) {
    // Normalize to 0, 90, 180, 270
    degrees = ((degrees % 360) + 360) % 360;

    if (degrees == 90 || degrees == 270) {
        // Swap guest dimensions for landscape
        setGuestResolution(guest_height_, guest_width_);
    } else {
        setGuestResolution(guest_width_, guest_height_);
    }

    recalcViewport();
    update();
}

void DisplayWidget::injectKey(uint16_t keycode, bool pressed) {
    emit keyInput(keycode, pressed);
}

} // namespace rex::gui
