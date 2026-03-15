#include "input_handler.h"

#include <Qt>
#include <algorithm>
#include <cmath>

namespace rex::gui {

InputHandler::InputHandler() {
    contacts_.resize(kMaxTouchSlots);
    buildKeyMap();
    buildGamingKeyMap();
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void InputHandler::setGuestResolution(uint32_t width, uint32_t height) {
    guest_width_  = width;
    guest_height_ = height;
}

void InputHandler::setViewport(int x, int y, int width, int height) {
    viewport_x_ = x;
    viewport_y_ = y;
    viewport_w_ = width;
    viewport_h_ = height;
}

void InputHandler::setGamingMode(bool enabled) {
    gaming_mode_ = enabled;
}

// ---------------------------------------------------------------------------
// Key mapping
// ---------------------------------------------------------------------------

void InputHandler::buildKeyMap() {
    // Letters
    for (int i = 0; i < 26; ++i) {
        // Qt::Key_A = 0x41, linux KEY_A = 30, KEY_B = 48, etc.
        static const uint16_t linux_letters[] = {
            linux_input::KEY_A, linux_input::KEY_B, linux_input::KEY_C,
            linux_input::KEY_D, linux_input::KEY_E, linux_input::KEY_F,
            linux_input::KEY_G, linux_input::KEY_H, linux_input::KEY_I,
            linux_input::KEY_J, linux_input::KEY_K, linux_input::KEY_L,
            linux_input::KEY_M, linux_input::KEY_N, linux_input::KEY_O,
            linux_input::KEY_P, linux_input::KEY_Q, linux_input::KEY_R,
            linux_input::KEY_S, linux_input::KEY_T, linux_input::KEY_U,
            linux_input::KEY_V, linux_input::KEY_W, linux_input::KEY_X,
            linux_input::KEY_Y, linux_input::KEY_Z
        };
        key_map_[Qt::Key_A + i] = linux_letters[i];
    }

    // Numbers
    key_map_[Qt::Key_0] = linux_input::KEY_0;
    key_map_[Qt::Key_1] = linux_input::KEY_1;
    key_map_[Qt::Key_2] = linux_input::KEY_2;
    key_map_[Qt::Key_3] = linux_input::KEY_3;
    key_map_[Qt::Key_4] = linux_input::KEY_4;
    key_map_[Qt::Key_5] = linux_input::KEY_5;
    key_map_[Qt::Key_6] = linux_input::KEY_6;
    key_map_[Qt::Key_7] = linux_input::KEY_7;
    key_map_[Qt::Key_8] = linux_input::KEY_8;
    key_map_[Qt::Key_9] = linux_input::KEY_9;

    // Function keys
    key_map_[Qt::Key_F1]  = linux_input::KEY_F1;
    key_map_[Qt::Key_F2]  = linux_input::KEY_F2;
    key_map_[Qt::Key_F3]  = linux_input::KEY_F3;
    key_map_[Qt::Key_F4]  = linux_input::KEY_F4;
    key_map_[Qt::Key_F5]  = linux_input::KEY_F5;
    key_map_[Qt::Key_F6]  = linux_input::KEY_F6;
    key_map_[Qt::Key_F7]  = linux_input::KEY_F7;
    key_map_[Qt::Key_F8]  = linux_input::KEY_F8;
    key_map_[Qt::Key_F9]  = linux_input::KEY_F9;
    key_map_[Qt::Key_F10] = linux_input::KEY_F10;
    key_map_[Qt::Key_F11] = linux_input::KEY_F11;
    key_map_[Qt::Key_F12] = linux_input::KEY_F12;

    // Modifiers
    key_map_[Qt::Key_Shift]    = linux_input::KEY_LEFTSHIFT;
    key_map_[Qt::Key_Control]  = linux_input::KEY_LEFTCTRL;
    key_map_[Qt::Key_Alt]      = linux_input::KEY_LEFTALT;
    key_map_[Qt::Key_CapsLock] = linux_input::KEY_CAPSLOCK;

    // Navigation
    key_map_[Qt::Key_Up]       = linux_input::KEY_UP;
    key_map_[Qt::Key_Down]     = linux_input::KEY_DOWN;
    key_map_[Qt::Key_Left]     = linux_input::KEY_LEFT;
    key_map_[Qt::Key_Right]    = linux_input::KEY_RIGHT;
    key_map_[Qt::Key_Home]     = linux_input::KEY_HOME;
    key_map_[Qt::Key_Delete]   = linux_input::KEY_DELETE;

    // Common
    key_map_[Qt::Key_Return]    = linux_input::KEY_ENTER;
    key_map_[Qt::Key_Enter]     = linux_input::KEY_ENTER;
    key_map_[Qt::Key_Escape]    = linux_input::KEY_ESC;
    key_map_[Qt::Key_Backspace] = linux_input::KEY_BACKSPACE;
    key_map_[Qt::Key_Tab]       = linux_input::KEY_TAB;
    key_map_[Qt::Key_Space]     = linux_input::KEY_SPACE;

    // Symbols
    key_map_[Qt::Key_Minus]       = linux_input::KEY_MINUS;
    key_map_[Qt::Key_Equal]       = linux_input::KEY_EQUAL;
    key_map_[Qt::Key_BracketLeft] = linux_input::KEY_LEFTBRACE;
    key_map_[Qt::Key_BracketRight]= linux_input::KEY_RIGHTBRACE;
    key_map_[Qt::Key_Backslash]   = linux_input::KEY_BACKSLASH;
    key_map_[Qt::Key_Semicolon]   = linux_input::KEY_SEMICOLON;
    key_map_[Qt::Key_Apostrophe]  = linux_input::KEY_APOSTROPHE;
    key_map_[Qt::Key_QuoteLeft]   = linux_input::KEY_GRAVE;
    key_map_[Qt::Key_Comma]       = linux_input::KEY_COMMA;
    key_map_[Qt::Key_Period]      = linux_input::KEY_DOT;
    key_map_[Qt::Key_Slash]       = linux_input::KEY_SLASH;

    // Android nav buttons
    key_map_[Qt::Key_Back]   = linux_input::KEY_BACK;
    key_map_[Qt::Key_Menu]   = linux_input::KEY_MENU;
    key_map_[Qt::Key_Search] = linux_input::KEY_SEARCH;

    // Volume
    key_map_[Qt::Key_VolumeUp]   = linux_input::KEY_VOLUMEUP;
    key_map_[Qt::Key_VolumeDown] = linux_input::KEY_VOLUMEDOWN;
}

void InputHandler::buildGamingKeyMap() {
    // WASD -> D-pad mapping for gaming mode
    gaming_key_map_[Qt::Key_W] = linux_input::KEY_DPAD_UP;
    gaming_key_map_[Qt::Key_A] = linux_input::KEY_DPAD_LEFT;
    gaming_key_map_[Qt::Key_S] = linux_input::KEY_DPAD_DOWN;
    gaming_key_map_[Qt::Key_D] = linux_input::KEY_DPAD_RIGHT;
}

uint16_t InputHandler::mapQtKeyToLinux(int qt_key) const {
    // In gaming mode, check gaming overrides first
    if (gaming_mode_) {
        auto git = gaming_key_map_.find(qt_key);
        if (git != gaming_key_map_.end()) {
            return git->second;
        }
    }

    auto it = key_map_.find(qt_key);
    if (it != key_map_.end()) {
        return it->second;
    }
    return linux_input::KEY_RESERVED;
}

// ---------------------------------------------------------------------------
// Coordinate mapping
// ---------------------------------------------------------------------------

bool InputHandler::mapToGuest(const QPointF& host_pos,
                              int32_t& out_guest_x, int32_t& out_guest_y) const {
    if (viewport_w_ <= 0 || viewport_h_ <= 0) {
        return false;
    }

    // Compute position relative to viewport
    double rel_x = host_pos.x() - viewport_x_;
    double rel_y = host_pos.y() - viewport_y_;

    // Check if inside viewport
    if (rel_x < 0 || rel_y < 0 ||
        rel_x >= viewport_w_ || rel_y >= viewport_h_) {
        return false;
    }

    // Scale to guest resolution
    double scale_x = static_cast<double>(guest_width_)  / viewport_w_;
    double scale_y = static_cast<double>(guest_height_) / viewport_h_;

    out_guest_x = static_cast<int32_t>(std::round(rel_x * scale_x));
    out_guest_y = static_cast<int32_t>(std::round(rel_y * scale_y));

    // Clamp to guest bounds
    out_guest_x = std::clamp(out_guest_x, int32_t{0},
                             static_cast<int32_t>(guest_width_ - 1));
    out_guest_y = std::clamp(out_guest_y, int32_t{0},
                             static_cast<int32_t>(guest_height_ - 1));

    return true;
}

// ---------------------------------------------------------------------------
// Mouse event handlers
// ---------------------------------------------------------------------------

TouchContact InputHandler::handleMousePress(const QPointF& host_pos) {
    TouchContact contact;
    contact.id = next_tracking_id_++;

    if (mapToGuest(host_pos, contact.guest_x, contact.guest_y)) {
        contact.pressure = 255;
        contact.active = true;
    }

    mouse_down_ = true;
    contacts_[0] = contact;
    return contact;
}

TouchContact InputHandler::handleMouseMove(const QPointF& host_pos) {
    if (!mouse_down_) {
        return {};
    }

    TouchContact& contact = contacts_[0];
    if (mapToGuest(host_pos, contact.guest_x, contact.guest_y)) {
        contact.pressure = 255;
        contact.active = true;
    }
    return contact;
}

TouchContact InputHandler::handleMouseRelease(const QPointF& host_pos) {
    TouchContact& contact = contacts_[0];
    mapToGuest(host_pos, contact.guest_x, contact.guest_y);
    contact.pressure = 0;
    contact.active = false;

    mouse_down_ = false;
    return contact;
}

// ---------------------------------------------------------------------------
// Key event handlers
// ---------------------------------------------------------------------------

uint16_t InputHandler::handleKeyPress(int qt_key) {
    return mapQtKeyToLinux(qt_key);
}

uint16_t InputHandler::handleKeyRelease(int qt_key) {
    return mapQtKeyToLinux(qt_key);
}

// ---------------------------------------------------------------------------
// Multi-touch
// ---------------------------------------------------------------------------

std::vector<TouchContact> InputHandler::handleTouchEvent(
    const std::vector<std::pair<int, QPointF>>& touch_points,
    bool is_release)
{
    // Deactivate all contacts first
    for (auto& c : contacts_) {
        c.active = false;
    }

    for (const auto& [point_id, pos] : touch_points) {
        // Find existing slot for this point ID or allocate new one
        int slot = -1;
        for (int i = 0; i < kMaxTouchSlots; ++i) {
            if (contacts_[i].id == point_id) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            // Allocate a free slot
            for (int i = 0; i < kMaxTouchSlots; ++i) {
                if (!contacts_[i].active && contacts_[i].id < 0) {
                    slot = i;
                    break;
                }
            }
        }
        if (slot < 0) {
            continue; // No free slots
        }

        TouchContact& contact = contacts_[slot];
        contact.id = point_id;

        if (is_release) {
            contact.pressure = 0;
            contact.active = false;
            contact.id = -1;
        } else {
            if (mapToGuest(pos, contact.guest_x, contact.guest_y)) {
                contact.pressure = 255;
                contact.active = true;
            }
        }
    }

    // Return only active contacts
    std::vector<TouchContact> result;
    for (const auto& c : contacts_) {
        if (c.active) {
            result.push_back(c);
        }
    }
    return result;
}

} // namespace rex::gui
