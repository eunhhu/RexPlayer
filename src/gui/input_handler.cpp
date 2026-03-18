#include "input_handler.h"
#include "../vnc/vnc_client.h"

namespace rex::gui {

InputHandler::InputHandler(QObject* parent) : QObject(parent) {}

void InputHandler::setVncClient(rex::vnc::VncClient* vnc) {
    vnc_ = vnc;
}

bool InputHandler::eventFilter(QObject* obj, QEvent* event) {
    switch (event->type()) {
        case QEvent::KeyPress:
            handleKeyPress(static_cast<QKeyEvent*>(event));
            return true;
        case QEvent::KeyRelease:
            handleKeyRelease(static_cast<QKeyEvent*>(event));
            return true;
        case QEvent::MouseButtonPress:
            handleMousePress(static_cast<QMouseEvent*>(event));
            return true;
        case QEvent::MouseMove:
            handleMouseMove(static_cast<QMouseEvent*>(event));
            return true;
        case QEvent::MouseButtonRelease:
            handleMouseRelease(static_cast<QMouseEvent*>(event));
            return true;
        default:
            return QObject::eventFilter(obj, event);
    }
}

void InputHandler::handleKeyPress(QKeyEvent* event) {
    if (!vnc_ || event->isAutoRepeat()) return;
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
    }
    vnc_->sendKeyEvent(true, keysym);
}

void InputHandler::handleKeyRelease(QKeyEvent* event) {
    if (!vnc_ || event->isAutoRepeat()) return;
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
    }
    vnc_->sendKeyEvent(false, keysym);
}

void InputHandler::handleMousePress(QMouseEvent* event) {
    if (!vnc_) return;
    uint8_t buttons = 0;
    if (event->button() == Qt::LeftButton) buttons |= 1;
    if (event->button() == Qt::MiddleButton) buttons |= 2;
    if (event->button() == Qt::RightButton) buttons |= 4;
    vnc_->sendPointerEvent(event->pos().x(), event->pos().y(), buttons);
}

void InputHandler::handleMouseMove(QMouseEvent* event) {
    if (!vnc_) return;
    uint8_t buttons = 0;
    if (event->buttons() & Qt::LeftButton) buttons |= 1;
    vnc_->sendPointerEvent(event->pos().x(), event->pos().y(), buttons);
}

void InputHandler::handleMouseRelease(QMouseEvent* event) {
    if (!vnc_) return;
    vnc_->sendPointerEvent(event->pos().x(), event->pos().y(), 0);
}

} // namespace rex::gui
