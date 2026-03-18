#include "spice_input.h"

// Include GLib/SPICE headers only in .cpp — avoids Qt "signals" macro conflict
#undef signals
#include <spice-client.h>
#define signals Q_SIGNALS

#include <cstdio>

namespace rex::spice {

SpiceInput::SpiceInput(QObject* parent) : QObject(parent) {}

SpiceInput::~SpiceInput() {
    detachChannel();
}

void SpiceInput::attachChannel(SpiceInputsChannel* channel) {
    channel_ = channel;
    fprintf(stderr, "spice: input channel attached\n");
}

void SpiceInput::detachChannel() {
    channel_ = nullptr;
}

void SpiceInput::sendKeyPress(int scancode) {
    if (!channel_) return;
    spice_inputs_key_press(channel_, scancode);
}

void SpiceInput::sendKeyRelease(int scancode) {
    if (!channel_) return;
    spice_inputs_key_release(channel_, scancode);
}

void SpiceInput::sendMouseMove(int x, int y) {
    if (!channel_) return;
    spice_inputs_position(channel_, x, y, 0,
                          SPICE_MOUSE_BUTTON_MASK_LEFT);
}

void SpiceInput::sendMousePress(int button) {
    if (!channel_) return;
    spice_inputs_button_press(channel_, button,
                               SPICE_MOUSE_BUTTON_MASK_LEFT);
}

void SpiceInput::sendMouseRelease(int button) {
    if (!channel_) return;
    spice_inputs_button_release(channel_, button, 0);
}

int SpiceInput::qtKeyToScancode(int qtKey) {
    switch (qtKey) {
        case Qt::Key_Escape:    return 1;
        case Qt::Key_1:         return 2;
        case Qt::Key_2:         return 3;
        case Qt::Key_3:         return 4;
        case Qt::Key_4:         return 5;
        case Qt::Key_5:         return 6;
        case Qt::Key_6:         return 7;
        case Qt::Key_7:         return 8;
        case Qt::Key_8:         return 9;
        case Qt::Key_9:         return 10;
        case Qt::Key_0:         return 11;
        case Qt::Key_Q:         return 16;
        case Qt::Key_W:         return 17;
        case Qt::Key_E:         return 18;
        case Qt::Key_R:         return 19;
        case Qt::Key_T:         return 20;
        case Qt::Key_Y:         return 21;
        case Qt::Key_A:         return 30;
        case Qt::Key_S:         return 31;
        case Qt::Key_D:         return 32;
        case Qt::Key_F:         return 33;
        case Qt::Key_Space:     return 57;
        case Qt::Key_Return:    return 28;
        case Qt::Key_Backspace: return 14;
        case Qt::Key_Tab:       return 15;
        case Qt::Key_Up:        return 103;
        case Qt::Key_Down:      return 108;
        case Qt::Key_Left:      return 105;
        case Qt::Key_Right:     return 106;
        case Qt::Key_Home:      return 102;
        case Qt::Key_End:       return 107;
        default:                return 0;
    }
}

} // namespace rex::spice
