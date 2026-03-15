#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <QPointF>

namespace rex::gui {

/// Linux input event codes (subset used for Android)
namespace linux_input {
    // Key codes (linux/input-event-codes.h)
    constexpr uint16_t KEY_RESERVED     = 0;
    constexpr uint16_t KEY_ESC          = 1;
    constexpr uint16_t KEY_1            = 2;
    constexpr uint16_t KEY_2            = 3;
    constexpr uint16_t KEY_3            = 4;
    constexpr uint16_t KEY_4            = 5;
    constexpr uint16_t KEY_5            = 6;
    constexpr uint16_t KEY_6            = 7;
    constexpr uint16_t KEY_7            = 8;
    constexpr uint16_t KEY_8            = 9;
    constexpr uint16_t KEY_9            = 10;
    constexpr uint16_t KEY_0            = 11;
    constexpr uint16_t KEY_MINUS        = 12;
    constexpr uint16_t KEY_EQUAL        = 13;
    constexpr uint16_t KEY_BACKSPACE    = 14;
    constexpr uint16_t KEY_TAB          = 15;
    constexpr uint16_t KEY_Q            = 16;
    constexpr uint16_t KEY_W            = 17;
    constexpr uint16_t KEY_E            = 18;
    constexpr uint16_t KEY_R            = 19;
    constexpr uint16_t KEY_T            = 20;
    constexpr uint16_t KEY_Y            = 21;
    constexpr uint16_t KEY_U            = 22;
    constexpr uint16_t KEY_I            = 23;
    constexpr uint16_t KEY_O            = 24;
    constexpr uint16_t KEY_P            = 25;
    constexpr uint16_t KEY_LEFTBRACE    = 26;
    constexpr uint16_t KEY_RIGHTBRACE   = 27;
    constexpr uint16_t KEY_ENTER        = 28;
    constexpr uint16_t KEY_LEFTCTRL     = 29;
    constexpr uint16_t KEY_A            = 30;
    constexpr uint16_t KEY_S            = 31;
    constexpr uint16_t KEY_D            = 32;
    constexpr uint16_t KEY_F            = 33;
    constexpr uint16_t KEY_G            = 34;
    constexpr uint16_t KEY_H            = 35;
    constexpr uint16_t KEY_J            = 36;
    constexpr uint16_t KEY_K            = 37;
    constexpr uint16_t KEY_L            = 38;
    constexpr uint16_t KEY_SEMICOLON    = 39;
    constexpr uint16_t KEY_APOSTROPHE   = 40;
    constexpr uint16_t KEY_GRAVE        = 41;
    constexpr uint16_t KEY_LEFTSHIFT    = 42;
    constexpr uint16_t KEY_BACKSLASH    = 43;
    constexpr uint16_t KEY_Z            = 44;
    constexpr uint16_t KEY_X            = 45;
    constexpr uint16_t KEY_C            = 46;
    constexpr uint16_t KEY_V            = 47;
    constexpr uint16_t KEY_B            = 48;
    constexpr uint16_t KEY_N            = 49;
    constexpr uint16_t KEY_M            = 50;
    constexpr uint16_t KEY_COMMA        = 51;
    constexpr uint16_t KEY_DOT          = 52;
    constexpr uint16_t KEY_SLASH        = 53;
    constexpr uint16_t KEY_RIGHTSHIFT   = 54;
    constexpr uint16_t KEY_LEFTALT      = 56;
    constexpr uint16_t KEY_SPACE        = 57;
    constexpr uint16_t KEY_CAPSLOCK     = 58;
    constexpr uint16_t KEY_F1           = 59;
    constexpr uint16_t KEY_F2           = 60;
    constexpr uint16_t KEY_F3           = 61;
    constexpr uint16_t KEY_F4           = 62;
    constexpr uint16_t KEY_F5           = 63;
    constexpr uint16_t KEY_F6           = 64;
    constexpr uint16_t KEY_F7           = 65;
    constexpr uint16_t KEY_F8           = 66;
    constexpr uint16_t KEY_F9           = 67;
    constexpr uint16_t KEY_F10          = 68;
    constexpr uint16_t KEY_F11          = 87;
    constexpr uint16_t KEY_F12          = 88;
    constexpr uint16_t KEY_UP           = 103;
    constexpr uint16_t KEY_LEFT         = 105;
    constexpr uint16_t KEY_RIGHT        = 106;
    constexpr uint16_t KEY_DOWN         = 108;
    constexpr uint16_t KEY_DELETE       = 111;

    // Android-specific key codes
    constexpr uint16_t KEY_HOME         = 102;
    constexpr uint16_t KEY_BACK         = 158;
    constexpr uint16_t KEY_MENU         = 139;
    constexpr uint16_t KEY_POWER        = 116;
    constexpr uint16_t KEY_VOLUMEUP     = 115;
    constexpr uint16_t KEY_VOLUMEDOWN   = 114;
    constexpr uint16_t KEY_SEARCH       = 217;

    // D-pad (for WASD gaming mapping)
    constexpr uint16_t KEY_DPAD_UP      = 103; // same as KEY_UP
    constexpr uint16_t KEY_DPAD_DOWN    = 108; // same as KEY_DOWN
    constexpr uint16_t KEY_DPAD_LEFT    = 105; // same as KEY_LEFT
    constexpr uint16_t KEY_DPAD_RIGHT   = 106; // same as KEY_RIGHT

    // Touch event types (ABS_MT_*)
    constexpr uint16_t ABS_MT_SLOT          = 0x2F;
    constexpr uint16_t ABS_MT_TOUCH_MAJOR   = 0x30;
    constexpr uint16_t ABS_MT_POSITION_X    = 0x35;
    constexpr uint16_t ABS_MT_POSITION_Y    = 0x36;
    constexpr uint16_t ABS_MT_TRACKING_ID   = 0x39;
    constexpr uint16_t ABS_MT_PRESSURE      = 0x3A;

} // namespace linux_input

/// Represents a single touch contact point
struct TouchContact {
    int id = -1;              ///< Tracking ID (-1 = inactive)
    int32_t guest_x = 0;     ///< X coordinate in guest resolution
    int32_t guest_y = 0;     ///< Y coordinate in guest resolution
    int32_t pressure = 0;    ///< Pressure (0 = released)
    bool active = false;
};

/// InputHandler maps Qt input events to Linux/Android input event codes.
///
/// It translates Qt key codes to Linux KEY_* constants, maps host mouse
/// coordinates to guest touch coordinates with proper resolution scaling,
/// and handles multi-touch events.
class InputHandler {
public:
    InputHandler();
    ~InputHandler() = default;

    // --- Configuration ---

    /// Set the guest display resolution for coordinate mapping
    void setGuestResolution(uint32_t width, uint32_t height);

    /// Set the viewport rect within the host widget (for letterboxing)
    void setViewport(int x, int y, int width, int height);

    /// Enable/disable WASD-to-dpad gaming mode
    void setGamingMode(bool enabled);
    bool isGamingMode() const { return gaming_mode_; }

    // --- Key mapping ---

    /// Map a Qt key code to a Linux input key code.
    /// @return Linux key code, or KEY_RESERVED (0) if unmapped
    uint16_t mapQtKeyToLinux(int qt_key) const;

    // --- Mouse/Touch coordinate mapping ---

    /// Map a host widget position to guest touch coordinates.
    /// Returns false if the position is outside the guest viewport.
    bool mapToGuest(const QPointF& host_pos,
                    int32_t& out_guest_x, int32_t& out_guest_y) const;

    // --- Event handlers (produce input events to inject into guest) ---

    /// Handle mouse press. Returns the generated touch contact.
    TouchContact handleMousePress(const QPointF& host_pos);

    /// Handle mouse move (drag). Returns the updated touch contact.
    TouchContact handleMouseMove(const QPointF& host_pos);

    /// Handle mouse release. Returns the final touch contact.
    TouchContact handleMouseRelease(const QPointF& host_pos);

    /// Handle a key press. Returns the Linux key code.
    uint16_t handleKeyPress(int qt_key);

    /// Handle a key release. Returns the Linux key code.
    uint16_t handleKeyRelease(int qt_key);

    /// Handle a multi-touch event. Returns all active touch contacts.
    std::vector<TouchContact> handleTouchEvent(
        const std::vector<std::pair<int, QPointF>>& touch_points,
        bool is_release);

    // --- State queries ---

    /// Get all currently active touch contacts
    const std::vector<TouchContact>& activeContacts() const { return contacts_; }

    /// Whether any touch/mouse is currently active
    bool isTouching() const { return mouse_down_; }

private:
    void buildKeyMap();
    void buildGamingKeyMap();

    // Key mapping tables
    std::unordered_map<int, uint16_t> key_map_;
    std::unordered_map<int, uint16_t> gaming_key_map_;

    // Guest display resolution
    uint32_t guest_width_  = 1080;
    uint32_t guest_height_ = 1920;

    // Viewport within the host widget (letterboxed area)
    int viewport_x_ = 0;
    int viewport_y_ = 0;
    int viewport_w_ = 0;
    int viewport_h_ = 0;

    // Mouse/touch state
    bool mouse_down_ = false;
    bool gaming_mode_ = false;
    int next_tracking_id_ = 0;

    // Multi-touch contacts (slot 0 is used for mouse)
    std::vector<TouchContact> contacts_;
    static constexpr int kMaxTouchSlots = 10;
};

} // namespace rex::gui
