#include "display_widget.h"
#include "../vnc/vnc_client.h"
#include "../emu/grpc_display.h"
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <algorithm>

namespace rex::gui {

DisplayWidget::DisplayWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 480);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void DisplayWidget::setVncClient(rex::vnc::VncClient* vnc) {
    vnc_ = vnc;
    if (vnc) {
        connect(vnc, &rex::vnc::VncClient::frameReady,
                this, &DisplayWidget::onFrameReady);
    }
}

void DisplayWidget::setGrpcDisplay(rex::emu::GrpcDisplay* grpc) {
    grpc_ = grpc;
    if (grpc) {
        connect(grpc, &rex::emu::GrpcDisplay::frameReady,
                this, &DisplayWidget::onFrameReady);
    }
}

void DisplayWidget::onFrameReady() {
    if (grpc_) {
        current_frame_ = grpc_->currentFrame();
        update();
        return;
    }
    if (vnc_) {
        current_frame_ = vnc_->currentFrame();
        update();
    }
}

QRect DisplayWidget::computeViewport() const {
    if (current_frame_.isNull()) return rect();

    double scale_x = static_cast<double>(width()) / current_frame_.width();
    double scale_y = static_cast<double>(height()) / current_frame_.height();
    double scale = std::min(scale_x, scale_y);

    int vw = static_cast<int>(current_frame_.width() * scale);
    int vh = static_cast<int>(current_frame_.height() * scale);
    int vx = (width() - vw) / 2;
    int vy = (height() - vh) / 2;

    return {vx, vy, vw, vh};
}

QPoint DisplayWidget::mapToGuest(const QPoint& widgetPos) const {
    QRect vp = computeViewport();
    if (vp.width() <= 0 || vp.height() <= 0 || current_frame_.isNull())
        return {0, 0};

    int gx = (widgetPos.x() - vp.x()) * current_frame_.width() / vp.width();
    int gy = (widgetPos.y() - vp.y()) * current_frame_.height() / vp.height();
    gx = std::clamp(gx, 0, current_frame_.width() - 1);
    gy = std::clamp(gy, 0, current_frame_.height() - 1);
    return {gx, gy};
}

void DisplayWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if (!current_frame_.isNull()) {
        QRect viewport = computeViewport();
        painter.drawImage(viewport, current_frame_);
    }
}

void DisplayWidget::keyPressEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) return;
    if (grpc_) { grpc_->sendKey(event->key(), true); return; }
    if (!vnc_) return;
    uint32_t keysym = event->key();
    if (keysym >= Qt::Key_A && keysym <= Qt::Key_Z) {
        keysym = (event->modifiers() & Qt::ShiftModifier)
            ? keysym - Qt::Key_A + 0x41
            : keysym - Qt::Key_A + 0x61;
    } else if (keysym >= Qt::Key_0 && keysym <= Qt::Key_9) {
        keysym = keysym - Qt::Key_0 + 0x30;
    } else if (keysym == Qt::Key_Space) { keysym = 0x20;
    } else if (keysym == Qt::Key_Return) { keysym = 0xFF0D;
    } else if (keysym == Qt::Key_Escape) { keysym = 0xFF1B;
    } else if (keysym == Qt::Key_Backspace) { keysym = 0xFF08;
    } else if (keysym == Qt::Key_Tab) { keysym = 0xFF09;
    } else if (keysym == Qt::Key_Up) { keysym = 0xFF52;
    } else if (keysym == Qt::Key_Down) { keysym = 0xFF54;
    } else if (keysym == Qt::Key_Left) { keysym = 0xFF51;
    } else if (keysym == Qt::Key_Right) { keysym = 0xFF53;
    }
    vnc_->sendKeyEvent(true, keysym);
}

void DisplayWidget::keyReleaseEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) return;
    if (grpc_) { grpc_->sendKey(event->key(), false); return; }
    if (!vnc_) return;
    uint32_t keysym = event->key();
    if (keysym >= Qt::Key_A && keysym <= Qt::Key_Z) {
        keysym = (event->modifiers() & Qt::ShiftModifier)
            ? keysym - Qt::Key_A + 0x41
            : keysym - Qt::Key_A + 0x61;
    } else if (keysym >= Qt::Key_0 && keysym <= Qt::Key_9) {
        keysym = keysym - Qt::Key_0 + 0x30;
    } else if (keysym == Qt::Key_Space) { keysym = 0x20;
    } else if (keysym == Qt::Key_Return) { keysym = 0xFF0D;
    } else if (keysym == Qt::Key_Escape) { keysym = 0xFF1B;
    } else if (keysym == Qt::Key_Backspace) { keysym = 0xFF08;
    } else if (keysym == Qt::Key_Tab) { keysym = 0xFF09;
    } else if (keysym == Qt::Key_Up) { keysym = 0xFF52;
    } else if (keysym == Qt::Key_Down) { keysym = 0xFF54;
    } else if (keysym == Qt::Key_Left) { keysym = 0xFF51;
    } else if (keysym == Qt::Key_Right) { keysym = 0xFF53;
    }
    vnc_->sendKeyEvent(false, keysym);
}

void DisplayWidget::mousePressEvent(QMouseEvent* event) {
    if (grpc_) {
        QPoint g = mapToGuest(event->pos());
        grpc_->sendTouch(g.x(), g.y(), 0, true);
        return;
    }
    if (!vnc_) return;
    QPoint guest = mapToGuest(event->pos());
    uint8_t buttons = 0;
    if (event->button() == Qt::LeftButton) buttons |= 1;
    if (event->button() == Qt::MiddleButton) buttons |= 2;
    if (event->button() == Qt::RightButton) buttons |= 4;
    vnc_->sendPointerEvent(guest.x(), guest.y(), buttons);
}

void DisplayWidget::mouseMoveEvent(QMouseEvent* event) {
    if (grpc_ && (event->buttons() & Qt::LeftButton)) {
        QPoint g = mapToGuest(event->pos());
        grpc_->sendTouch(g.x(), g.y(), 0, true);
        return;
    }
    if (!vnc_) return;
    QPoint guest = mapToGuest(event->pos());
    uint8_t buttons = 0;
    if (event->buttons() & Qt::LeftButton) buttons |= 1;
    if (event->buttons() & Qt::MiddleButton) buttons |= 2;
    if (event->buttons() & Qt::RightButton) buttons |= 4;
    vnc_->sendPointerEvent(guest.x(), guest.y(), buttons);
}

void DisplayWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (grpc_) {
        QPoint g = mapToGuest(event->pos());
        grpc_->sendTouch(g.x(), g.y(), 0, false);
        return;
    }
    if (!vnc_) return;
    QPoint guest = mapToGuest(event->pos());
    vnc_->sendPointerEvent(guest.x(), guest.y(), 0);
}

} // namespace rex::gui
