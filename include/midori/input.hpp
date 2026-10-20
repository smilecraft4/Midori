#pragma once

#include "midori/base.hpp"
#include "midori/platform.hpp"
#include <cstdint>

namespace Midori {

using Device = std::uint8_t;
constexpr Device kDeviceSystem = 0; // keyboard and mouse
constexpr Device kDevicePen = 1;    // tablet pen tip
constexpr Device kDeviceEraser = 2; // tablet pen, eraser end

enum struct InputState : std::uint8_t {
    None,
    Press,
    Hold,
    Release,
};

enum struct Key : std::uint8_t {
    Unknown = 0,

    // Letters
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,

    // Numbers
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,

    // Function keys
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    F13,
    F14,
    F15,
    F16,
    F17,
    F18,
    F19,
    F20,
    F21,
    F22,
    F23,
    F24,

    // Navigation / editing
    Escape,
    Tab,
    CapsLock,
    Backspace,
    Enter,
    Space,

    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,

    Left,
    Right,
    Up,
    Down,

    // Punctuation
    Minus,
    Equal,
    LeftBracket,
    RightBracket,
    Backslash,
    Semicolon,
    Apostrophe,
    Grave,
    Comma,
    Period,
    Slash,

    // Keypad
    NumPad0,
    NumPad1,
    NumPad2,
    NumPad3,
    NumPad4,
    NumPad5,
    NumPad6,
    NumPad7,
    NumPad8,
    NumPad9,
    NumPadDecimal,
    NumPadDivide,
    NumPadMultiply,
    NumPadSubtract,
    NumPadAdd,
    NumPadEnter,

    // Locks / system
    NumLock,
    ScrollLock,
    PrintScreen,
    Pause,
    Menu,

    // Modifier keys
    LeftShift,
    RightShift,
    LeftCtrl,
    RightCtrl,
    LeftAlt,
    RightAlt,
    LeftSuper,
    RightSuper,
};

enum struct KeyMod : std::uint8_t {
    None = 0,

    LShift = 1 << 0,
    RShift = 1 << 1,

    LCtrl = 1 << 2,
    RCtrl = 1 << 3,

    LAlt = 1 << 4,
    RAlt = 1 << 5,

    LSuper = 1 << 6,
    RSuper = 1 << 7,

    Shift = LShift | RShift,
    Ctrl = LCtrl | RCtrl,
    Alt = LAlt | RAlt,
    Super = LSuper | RSuper,

    Mod = Shift | Ctrl | Alt | Super
};

template<>
inline constexpr bool kEnumFlags<KeyMod> = true;

enum struct Button : std::uint8_t {
    Unknown,

    Left,
    Middle,
    Right,
};

enum struct EventType : std::uint8_t {
    Unknown,

    WindowResize,
    WindowFocus,
    WindowDestroy,
    Key,
    Text,
    Button,
    Pointer,
    Wheel,
};

// Client-area size in physical pixels; 0x0 while minimized. Also sent when the DPI changes.
struct WindowResizeEvent {
    int newWidth;
    int newHeight;
    float dpiScale;
    Window window;
};

// Losing focus means every held key and button is released without an event.
struct WindowFocusEvent {
    Window window;
    bool focused;
};

// The window is going away; its id may be reused by the next WindowCreate.
struct WindowDestroyEvent {
    Window window;
};

struct KeyEvent {
    Window window;
    Device device;
    Key key;
    KeyMod keyMods;
    InputState inputState;
};

// One Unicode codepoint of typed text, after the keyboard layout and dead keys are applied.
struct TextEvent {
    char32_t codepoint;
    Window window;
};

struct ButtonEvent {
    Window window;
    Device device;
    Button button;
    InputState inputState;
};

// Position in client-area physical pixels, sub-pixel for the tablet. Pressure is 0..1 (mouse: 1 while
// a button is held). Tilt is the pen's lean projected on the tablet, -1..1 per axis (0 = upright),
// +x right and +y down like the position.
struct PointerEvent {
    float posX;
    float posY;
    float pressure;
    float tiltX;
    float tiltY;
    Window window;
    Device device;
};

// In wheel notches, fractional for precise touchpads. +x scrolls right, +y scrolls up (away from the user).
struct WheelEvent {
    float deltaX;
    float deltaY;
    Window window;
    Device device;
};

struct Event {
    EventType type{};
    union {
        KeyEvent key;
        TextEvent text;
        ButtonEvent button;
        PointerEvent pointer;
        WheelEvent wheel;
        WindowResizeEvent windowResize;
        WindowFocusEvent windowFocus;
        WindowDestroyEvent windowDestroy;
    };
};

} // namespace Midori
