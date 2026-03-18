#include "input_handler.h"
#include "../spice/spice_input.h"

namespace rex::gui {

InputHandler::InputHandler(QObject* parent) : QObject(parent) {}

void InputHandler::setSpiceInput(rex::spice::SpiceInput* input) {
    spice_input_ = input;
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
    if (!spice_input_ || event->isAutoRepeat()) return;
    int sc = rex::spice::SpiceInput::qtKeyToScancode(event->key());
    if (sc > 0) spice_input_->sendKeyPress(sc);
}

void InputHandler::handleKeyRelease(QKeyEvent* event) {
    if (!spice_input_ || event->isAutoRepeat()) return;
    int sc = rex::spice::SpiceInput::qtKeyToScancode(event->key());
    if (sc > 0) spice_input_->sendKeyRelease(sc);
}

void InputHandler::handleMousePress(QMouseEvent* event) {
    if (!spice_input_) return;
    auto pos = event->pos();
    spice_input_->sendMouseMove(pos.x(), pos.y());
    spice_input_->sendMousePress(1);
}

void InputHandler::handleMouseMove(QMouseEvent* event) {
    if (!spice_input_) return;
    auto pos = event->pos();
    spice_input_->sendMouseMove(pos.x(), pos.y());
}

void InputHandler::handleMouseRelease(QMouseEvent*) {
    if (!spice_input_) return;
    spice_input_->sendMouseRelease(1);
}

} // namespace rex::gui
