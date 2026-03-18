#include "display_widget.h"
#include "../spice/spice_display.h"
#include "../spice/spice_input.h"
#include "../spice/spice_client.h"
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>

namespace rex::gui {

DisplayWidget::DisplayWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 480);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void DisplayWidget::setSpiceDisplay(rex::spice::SpiceDisplay* display) {
    spice_display_ = display;
    if (display) {
        connect(display, &rex::spice::SpiceDisplay::frameReady,
                this, &DisplayWidget::onFrameReady);
    }
}

void DisplayWidget::onFrameReady() {
    if (spice_display_) {
        current_frame_ = spice_display_->currentFrame();
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
    // Key input will be handled by InputHandler in future tasks
    QWidget::keyPressEvent(event);
}

void DisplayWidget::keyReleaseEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) return;
    QWidget::keyReleaseEvent(event);
}

void DisplayWidget::mousePressEvent(QMouseEvent* event) {
    Q_UNUSED(event);
}

void DisplayWidget::mouseMoveEvent(QMouseEvent* event) {
    Q_UNUSED(event);
}

void DisplayWidget::mouseReleaseEvent(QMouseEvent* event) {
    Q_UNUSED(event);
}

} // namespace rex::gui
