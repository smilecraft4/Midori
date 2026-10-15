#include "win32_platform.hpp"

#include "midori/input.hpp"

#include <windowsx.h>

#include <utility>

namespace Midori {

namespace {

// Mouse messages Windows synthesizes from pen or touch input carry this signature.
constexpr LPARAM kPenSignatureMask = 0xFFFFFF00;
constexpr LPARAM kPenSignature = 0xFF515700;

[[nodiscard]] Key KeyOffset(Key first, WPARAM offset) noexcept {
    return static_cast<Key>(std::to_underlying(first) + offset);
}

[[nodiscard]] Key KeyFromMessage(WPARAM vk, LPARAM lParam) noexcept {
    const bool extended = (lParam & (1 << 24)) != 0;
    if (vk >= 'A' && vk <= 'Z') {
        return KeyOffset(Key::A, vk - 'A');
    }
    if (vk >= '0' && vk <= '9') {
        return KeyOffset(Key::Num0, vk - '0');
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return KeyOffset(Key::F1, vk - VK_F1);
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return KeyOffset(Key::NumPad0, vk - VK_NUMPAD0);
    }
    switch (vk) {
        case VK_ESCAPE:
            return Key::Escape;
        case VK_TAB:
            return Key::Tab;
        case VK_CAPITAL:
            return Key::CapsLock;
        case VK_BACK:
            return Key::Backspace;
        case VK_RETURN:
            return extended ? Key::NumPadEnter : Key::Enter;
        case VK_SPACE:
            return Key::Space;
        case VK_INSERT:
            return Key::Insert;
        case VK_DELETE:
            return Key::Delete;
        case VK_HOME:
            return Key::Home;
        case VK_END:
            return Key::End;
        case VK_PRIOR:
            return Key::PageUp;
        case VK_NEXT:
            return Key::PageDown;
        case VK_LEFT:
            return Key::Left;
        case VK_RIGHT:
            return Key::Right;
        case VK_UP:
            return Key::Up;
        case VK_DOWN:
            return Key::Down;
        case VK_OEM_MINUS:
            return Key::Minus;
        case VK_OEM_PLUS:
            return Key::Equal;
        case VK_OEM_4:
            return Key::LeftBracket;
        case VK_OEM_6:
            return Key::RightBracket;
        case VK_OEM_5:
            return Key::Backslash;
        case VK_OEM_1:
            return Key::Semicolon;
        case VK_OEM_7:
            return Key::Apostrophe;
        case VK_OEM_3:
            return Key::Grave;
        case VK_OEM_COMMA:
            return Key::Comma;
        case VK_OEM_PERIOD:
            return Key::Period;
        case VK_OEM_2:
            return Key::Slash;
        case VK_DECIMAL:
            return Key::NumPadDecimal;
        case VK_DIVIDE:
            return Key::NumPadDivide;
        case VK_MULTIPLY:
            return Key::NumPadMultiply;
        case VK_SUBTRACT:
            return Key::NumPadSubtract;
        case VK_ADD:
            return Key::NumPadAdd;
        case VK_NUMLOCK:
            return Key::NumLock;
        case VK_SCROLL:
            return Key::ScrollLock;
        case VK_SNAPSHOT:
            return Key::PrintScreen;
        case VK_PAUSE:
            return Key::Pause;
        case VK_APPS:
            return Key::Menu;
        // Messages only say "shift", "control" or "alt": the scancode and extended bit tell the side.
        case VK_SHIFT: {
            const UINT scancode = (static_cast<UINT>(lParam) >> 16) & 0xFF;
            return MapVirtualKeyW(scancode, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? Key::RightShift : Key::LeftShift;
        }
        case VK_CONTROL:
            return extended ? Key::RightCtrl : Key::LeftCtrl;
        case VK_MENU:
            return extended ? Key::RightAlt : Key::LeftAlt;
        case VK_LWIN:
            return Key::LeftSuper;
        case VK_RWIN:
            return Key::RightSuper;
        default:
            return Key::Unknown;
    }
}

[[nodiscard]] KeyMod CurrentKeyMods() noexcept {
    constexpr struct {
        int vk;
        KeyMod mod;
    } kMods[] = {
        {.vk = VK_LSHIFT, .mod = KeyMod::LShift},  {.vk = VK_RSHIFT, .mod = KeyMod::RShift},
        {.vk = VK_LCONTROL, .mod = KeyMod::LCtrl}, {.vk = VK_RCONTROL, .mod = KeyMod::RCtrl},
        {.vk = VK_LMENU, .mod = KeyMod::LAlt},     {.vk = VK_RMENU, .mod = KeyMod::RAlt},
        {.vk = VK_LWIN, .mod = KeyMod::LSuper},    {.vk = VK_RWIN, .mod = KeyMod::RSuper},
    };
    KeyMod mods = KeyMod::None;
    for (const auto& [vk, mod] : kMods) {
        if (GetKeyState(vk) < 0) {
            mods |= mod;
        }
    }
    return mods;
}

void DispatchKey(Window window, WPARAM wParam, LPARAM lParam, bool down) noexcept {
    // Bit 30 is the key's previous state: down and down again is auto-repeat.
    InputState inputState = InputState::Release;
    if (down) {
        inputState = (lParam & (1 << 30)) != 0 ? InputState::Hold : InputState::Press;
    }
    Event event{};
    event.type = EventType::Key;
    event.key = KeyEvent{
        .window = window,
        .device = kDeviceSystem,
        .key = KeyFromMessage(wParam, lParam),
        .keyMods = CurrentKeyMods(),
        .inputState = inputState,
    };
    PlatformDispatch(event);
}

// The tablet reports the pen itself; the mouse messages it also generates would double every stroke.
[[nodiscard]] bool FromPen(Window window) noexcept {
    const Win32Window& slot = gPlatformState.windows[window];
    return slot.tablet && (slot.tabletProximity || (GetMessageExtraInfo() & kPenSignatureMask) == kPenSignature);
}

void HandleButton(Window window, Button button, bool down) noexcept {
    if (FromPen(window)) {
        return;
    }
    HWND hwnd = gPlatformState.windows[window].hwnd;
    // Keep receiving the release when it happens outside the window.
    if (down) {
        SetCapture(hwnd);
    } else if (GetCapture() == hwnd) {
        ReleaseCapture();
    }
    Event event{};
    event.type = EventType::Button;
    event.button = ButtonEvent{
        .window = window,
        .device = kDeviceSystem,
        .button = button,
        .inputState = down ? InputState::Press : InputState::Release,
    };
    PlatformDispatch(event);
}

void DispatchMouseMove(Window window, WPARAM wParam, LPARAM lParam) noexcept {
    Event event{};
    event.type = EventType::Pointer;
    event.pointer = PointerEvent{
        .posX = static_cast<float>(GET_X_LPARAM(lParam)),
        .posY = static_cast<float>(GET_Y_LPARAM(lParam)),
        .pressure = (wParam & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON)) ? 1.0f : 0.0f,
        .tiltX = 0.0f,
        .tiltY = 0.0f,
        .window = window,
        .device = kDeviceSystem,
    };
    PlatformDispatch(event);
}

} // namespace

bool InputHandleMessage(Window window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
    switch (message) {
        case WM_KEYDOWN:
            DispatchKey(window, wParam, lParam, true);
            return true;
        case WM_KEYUP:
            DispatchKey(window, wParam, lParam, false);
            return true;
        // Alt combinations arrive as system keys. They are consumed so Alt alone does not open the
        // (absent) window menu and steal the next key, except Alt+F4 which DefWindowProc turns into close.
        case WM_SYSKEYDOWN:
            DispatchKey(window, wParam, lParam, true);
            return wParam != VK_F4;
        case WM_SYSKEYUP:
            DispatchKey(window, wParam, lParam, false);
            return true;
        case WM_SYSCHAR:
            // No menu: swallowing it also stops the error beep on Alt+letter.
            return true;

        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            HandleButton(window, Button::Left, message == WM_LBUTTONDOWN);
            return true;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            HandleButton(window, Button::Middle, message == WM_MBUTTONDOWN);
            return true;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            HandleButton(window, Button::Right, message == WM_RBUTTONDOWN);
            return true;
        case WM_MOUSEMOVE:
            if (!FromPen(window)) {
                DispatchMouseMove(window, wParam, lParam);
            }
            return true;
        default:
            return false;
    }
}

} // namespace Midori
