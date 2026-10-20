#include "win32_platform.hpp"

// Packet layout: absolute positions, tip pressure, orientation (tilt), button bits and status (for
// the eraser bit). Buttons are absolute too: the current bit set, compared against the last packet.
#define PACKETDATA (PK_X | PK_Y | PK_NORMAL_PRESSURE | PK_ORIENTATION | PK_BUTTONS | PK_STATUS)
#define PACKETMODE 0
#include <pktdef.h>

#include "win32_graphics.hpp"

#include "midori/app.hpp"
#include "midori/base.hpp"
#include "midori/input.hpp"
#include "midori/log.hpp"
#include "midori/ui.hpp"
#include "win32_resource.h"

#include <windowsx.h>

#include <cmath>
#include <iterator>
#include <numbers>
#include <span>
#include <utility>

#include <tracy/Tracy.hpp>

namespace {

[[noreturn]] void BreakAndAbort() noexcept {
    if (IsDebuggerPresent()) {
        DebugBreak();
    }
    std::abort();
}
} // namespace

namespace Midori {

PlatformState gPlatformState{};

namespace {

constexpr wchar_t kWindowClassName[] = L"MidoriWindow";

// From <tpcshrd.h>: turn off the pen gestures that would delay or swallow strokes.
constexpr UINT kWmTabletQuerySystemGestureStatus = 0x02CC;
constexpr LRESULT kTabletDisableGestures = 0x00000001  // press and hold
                                         | 0x00000008  // pen tap feedback
                                         | 0x00000010  // pen barrel feedback
                                         | 0x00010000; // flicks

constexpr DWORD kWindowedStyle = WS_OVERLAPPEDWINDOW;

constexpr int kWintabQueueSize = 128;

void PlatformDispatch(Event event) noexcept {
    if (const auto result = AppEvent(event); !result) {
        MIDORI_LOG_ERROR("AppEvent failed: {}", ErrorMessage(result.error()));
    }
}

[[nodiscard]] bool WindowValid(Window window) noexcept {
    return window < kWindowMax && gPlatformState.windows[window].hwnd;
}

[[nodiscard]] Window WindowFromHwnd(HWND hwnd) noexcept {
    for (Window i = 0; i < kWindowMax; ++i) {
        if (gPlatformState.windows[i].hwnd == hwnd) {
            return i;
        }
    }
    return kWindowInvalid;
}

// Titles that do not fit are left empty rather than cut mid-character.
void Utf8ToWide(std::string_view text, std::span<wchar_t> out) noexcept {
    const int written = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                                            static_cast<int>(out.size() - 1));
    out[static_cast<std::size_t>(written)] = L'\0';
}

void DispatchResize(Window window) noexcept {
    const WindowState& state = gPlatformState.windows[window].state;
    Event event{};
    event.type = EventType::WindowResize;
    event.windowResize = WindowResizeEvent{
        .newWidth = state.width,
        .newHeight = state.height,
        .dpiScale = state.dpiScale,
        .window = window,
    };
    PlatformDispatch(event);
}

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

// WM_CHAR sends UTF-16 units: a codepoint above U+FFFF arrives as two messages. Control characters
// are not text; they arrive as key events.
void DispatchText(Window window, WPARAM wParam) noexcept {
    Win32Window& slot = gPlatformState.windows[window];
    const auto unit = static_cast<char32_t>(wParam);
    if (unit >= 0xD800 && unit <= 0xDBFF) {
        slot.highSurrogate = static_cast<wchar_t>(unit);
        return;
    }
    char32_t codepoint = unit;
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
        if (!slot.highSurrogate) {
            return;
        }
        codepoint = 0x10000 + ((static_cast<char32_t>(slot.highSurrogate) - 0xD800) << 10) + (unit - 0xDC00);
    }
    slot.highSurrogate = 0;
    if (codepoint < 0x20 || codepoint == 0x7F) {
        return;
    }
    Event event{};
    event.type = EventType::Text;
    event.text = TextEvent{.codepoint = codepoint, .window = window};
    PlatformDispatch(event);
}

void DispatchWheel(Window window, WPARAM wParam, bool horizontal) noexcept {
    const float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
    Event event{};
    event.type = EventType::Wheel;
    event.wheel = WheelEvent{
        .deltaX = horizontal ? delta : 0.0f,
        .deltaY = horizontal ? 0.0f : delta,
        .window = window,
        .device = kDeviceSystem,
    };
    PlatformDispatch(event);
}

void DispatchFocus(Window window, bool focused) noexcept {
    Event event{};
    event.type = EventType::WindowFocus;
    event.windowFocus = WindowFocusEvent{.window = window, .focused = focused};
    PlatformDispatch(event);
}

void DispatchDestroy(Window window) noexcept {
    Event event{};
    event.type = EventType::WindowDestroy;
    event.windowDestroy = WindowDestroyEvent{.window = window};
    PlatformDispatch(event);
}

// Keyboard and mouse messages. Returns true when the message was consumed.
[[nodiscard]] bool InputHandleMessage(Window window, UINT message, WPARAM wParam, LPARAM lParam) noexcept {
    ZoneScopedN("InputHandleMessage");
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
        case WM_CHAR:
            DispatchText(window, wParam);
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
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            DispatchWheel(window, wParam, message == WM_MOUSEHWHEEL);
            return true;
        default:
            return false;
    }
}

template<typename F>
[[nodiscard]] F WintabLoad(HMODULE dll, const char* name) noexcept {
    return reinterpret_cast<F>(GetProcAddress(dll, name));
}

void WintabTerminate() noexcept {
    if (gPlatformState.wintab.dll) {
        FreeLibrary(gPlatformState.wintab.dll);
    }
    gPlatformState.wintab = {};
}

void WintabUnavailable(const char* reason) noexcept {
    MIDORI_LOG_INFO("Wintab unavailable ({}): pen input falls back to the mouse", reason);
    WintabTerminate();
}

void WintabInit() noexcept {
    ZoneScopedN("WintabInit");
    Wintab& wintab = gPlatformState.wintab;
    wintab.dll = LoadLibraryW(L"Wintab32.dll");
    if (!wintab.dll) {
        WintabUnavailable("Wintab32.dll not found");
        return;
    }
    wintab.info = WintabLoad<decltype(wintab.info)>(wintab.dll, "WTInfoW");
    wintab.open = WintabLoad<decltype(wintab.open)>(wintab.dll, "WTOpenW");
    wintab.close = WintabLoad<decltype(wintab.close)>(wintab.dll, "WTClose");
    wintab.packetsGet = WintabLoad<decltype(wintab.packetsGet)>(wintab.dll, "WTPacketsGet");
    wintab.overlap = WintabLoad<decltype(wintab.overlap)>(wintab.dll, "WTOverlap");
    wintab.queueSizeSet = WintabLoad<decltype(wintab.queueSizeSet)>(wintab.dll, "WTQueueSizeSet");
    if (!wintab.info || !wintab.open || !wintab.close || !wintab.packetsGet || !wintab.overlap
        || !wintab.queueSizeSet) {
        WintabUnavailable("Wintab32.dll is missing entry points");
        return;
    }
    // WTInfo(0, 0, nullptr) is non-zero when the tablet service is running.
    if (!wintab.info(0, 0, nullptr)) {
        WintabUnavailable("no tablet service");
        return;
    }

    // The default system context keeps the system cursor following the pen.
    LOGCONTEXTW& context = wintab.context;
    if (!wintab.info(WTI_DEFSYSCTX, 0, &context)) {
        WintabUnavailable("no default context");
        return;
    }

    AXIS pressure{};
    wintab.info(WTI_DEVICES, DVC_NPRESSURE, &pressure);
    wintab.maxPressure = pressure.axMax > 0 ? static_cast<float>(pressure.axMax) : 1.0f;

    AXIS orientation[3]{};
    if (wintab.info(WTI_DEVICES, DVC_ORIENTATION, orientation) && orientation[0].axMax > 0
        && orientation[1].axMax > 0) {
        wintab.maxAzimuth = static_cast<float>(orientation[0].axMax);
        wintab.maxAltitude = static_cast<float>(orientation[1].axMax);
    }

    // The screen area the driver maps the tablet to, in screen pixels.
    wintab.screenX = static_cast<float>(context.lcSysOrgX);
    wintab.screenY = static_cast<float>(context.lcSysOrgY);
    wintab.screenWidth = static_cast<float>(context.lcSysExtX);
    wintab.screenHeight = static_cast<float>(context.lcSysExtY);

    // Report the full tablet resolution instead of screen pixels, for sub-pixel positions. A negative
    // Y extent flips the axis so Y grows downwards like the screen.
    context.lcOptions |= CXO_MESSAGES | CXO_SYSTEM;
    context.lcPktData = PACKETDATA;
    context.lcPktMode = PACKETMODE;
    context.lcMoveMask = PACKETDATA;
    context.lcBtnUpMask = context.lcBtnDnMask;
    context.lcOutOrgX = 0;
    context.lcOutOrgY = 0;
    context.lcOutExtX = context.lcInExtX;
    context.lcOutExtY = -context.lcInExtY;
    wintab.tabletWidth = static_cast<float>(context.lcInExtX);
    wintab.tabletHeight = static_cast<float>(context.lcInExtY);

    MIDORI_LOG_INFO("Wintab: {} pressure levels, tilt {}", pressure.axMax + 1, wintab.maxAltitude > 0 ? "yes" : "no");
}

void WintabOpen(Window window) noexcept {
    ZoneScopedN("WintabOpen");
    const Wintab& wintab = gPlatformState.wintab;
    if (!wintab.dll) {
        return;
    }
    Win32Window& slot = gPlatformState.windows[window];
    LOGCONTEXTW context = wintab.context;
    slot.tablet = wintab.open(slot.hwnd, &context, TRUE);
    if (!slot.tablet) {
        MIDORI_LOG_WARN("WTOpen failed for window {}: no pen input there", window);
        return;
    }
    // The default queue holds a handful of packets; a fast pen overflows it between messages.
    if (!wintab.queueSizeSet(slot.tablet, kWintabQueueSize)) {
        MIDORI_LOG_WARN("WTQueueSizeSet({}) failed: packets may be dropped", kWintabQueueSize);
    }
}

void WintabClose(Window window) noexcept {
    Win32Window& slot = gPlatformState.windows[window];
    if (slot.tablet) {
        gPlatformState.wintab.close(slot.tablet);
        slot.tablet = nullptr;
    }
}

void WintabActivate(Window window) noexcept {
    // The focused window's context goes on top, so it is the one receiving packets.
    const Win32Window& slot = gPlatformState.windows[window];
    if (slot.tablet) {
        gPlatformState.wintab.overlap(slot.tablet, TRUE);
    }
}

void WintabDispatchButton(Window window, Device device, Button button, bool down) noexcept {
    Event event{};
    event.type = EventType::Button;
    event.button = ButtonEvent{
        .window = window,
        .device = device,
        .button = button,
        .inputState = down ? InputState::Press : InputState::Release,
    };
    PlatformDispatch(event);
}

void WintabHandlePackets(Window window) noexcept {
    ZoneScopedN("WintabHandlePackets");
    const Wintab& wintab = gPlatformState.wintab;
    Win32Window& slot = gPlatformState.windows[window];
    if (!slot.tablet) {
        return;
    }

    // Drain the whole queue at once: one WT_PACKET can stand for many packets.
    PACKET packets[kWintabQueueSize];
    const int count = wintab.packetsGet(slot.tablet, kWintabQueueSize, packets);

    POINT clientOrigin{};
    ClientToScreen(slot.hwnd, &clientOrigin);
    const float originX = static_cast<float>(clientOrigin.x) - wintab.screenX;
    const float originY = static_cast<float>(clientOrigin.y) - wintab.screenY;
    const float scaleX = wintab.screenWidth / wintab.tabletWidth;
    const float scaleY = wintab.screenHeight / wintab.tabletHeight;

    for (int i = 0; i < count; ++i) {
        const PACKET& packet = packets[i];
        const Device device = (packet.pkStatus & TPS_INVERT) ? kDeviceEraser : kDevicePen;

        float tiltX = 0.0f;
        float tiltY = 0.0f;
        if (wintab.maxAltitude > 0.0f) {
            // Azimuth: clockwise from the tablet's top. Altitude: from the tablet's plane, 90 deg = upright.
            const float azimuth = static_cast<float>(packet.pkOrientation.orAzimuth) / wintab.maxAzimuth * 2.0f
                                * std::numbers::pi_v<float>;
            const float altitude = static_cast<float>(std::abs(packet.pkOrientation.orAltitude)) / wintab.maxAltitude
                                 * 0.5f * std::numbers::pi_v<float>;
            const float lean = std::cos(altitude);
            tiltX = lean * std::sin(azimuth);
            tiltY = -lean * std::cos(azimuth);
        }

        Event event{};
        event.type = EventType::Pointer;
        event.pointer = PointerEvent{
            .posX = (static_cast<float>(packet.pkX) * scaleX) - originX,
            .posY = (static_cast<float>(packet.pkY) * scaleY) - originY,
            .pressure = static_cast<float>(packet.pkNormalPressure) / wintab.maxPressure,
            .tiltX = tiltX,
            .tiltY = tiltY,
            .window = window,
            .device = device,
        };
        PlatformDispatch(event);

        // Bit 0 is the tip, bits 1 and 2 the barrel buttons.
        const DWORD changed = packet.pkButtons ^ slot.tabletButtons;
        constexpr Button kButtons[] = {Button::Left, Button::Right, Button::Middle};
        for (DWORD bit = 0; bit < std::size(kButtons); ++bit) {
            if (changed & (1u << bit)) {
                WintabDispatchButton(window, device, kButtons[bit], (packet.pkButtons & (1u << bit)) != 0);
            }
        }
        slot.tabletButtons = packet.pkButtons;
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    ZoneScopedN("WindowProc");
    ZoneValue(message);
    // WindowCreate passes the slot so every later message, WM_SIZE during creation included, finds it.
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        gPlatformState.windows[*static_cast<const Window*>(create->lpCreateParams)].hwnd = hwnd;
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    const Window window = WindowFromHwnd(hwnd);
    if (window == kWindowInvalid) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    Win32Window& slot = gPlatformState.windows[window];

    if (InputHandleMessage(window, message, wParam, lParam)) {
        return 0;
    }

    switch (message) {
        case WM_SIZE: {
            slot.state.width = LOWORD(lParam);
            slot.state.height = HIWORD(lParam);
            if (wParam == SIZE_MINIMIZED) {
                slot.state.displayMode = WindowDisplayMode::Minimized;
            } else if (wParam == SIZE_MAXIMIZED) {
                slot.state.displayMode = WindowDisplayMode::Maximized;
            } else if (wParam == SIZE_RESTORED && slot.state.displayMode != WindowDisplayMode::Fullscreened) {
                slot.state.displayMode = WindowDisplayMode::Normal;
            }
            if (const auto res = GraphicsWindowResize(window, LOWORD(lParam), HIWORD(lParam)); !res) {
                MIDORI_LOG_ERROR("GraphicsWindowResize failed: {}", ErrorMessage(res.error()));
            }
            DispatchResize(window);
            return 0;
        }
        case WM_MOVE:
            slot.state.x = static_cast<short>(LOWORD(lParam));
            slot.state.y = static_cast<short>(HIWORD(lParam));
            return 0;
        case WM_SHOWWINDOW:
            if (!wParam) {
                slot.state.displayMode = WindowDisplayMode::Hidden;
            } else if (slot.state.displayMode == WindowDisplayMode::Hidden) {
                slot.state.displayMode = WindowDisplayMode::Normal;
            }
            break;
        case WM_DPICHANGED: {
            // Take the size Windows suggests for the new DPI; the WM_SIZE it causes reports it.
            slot.state.dpiScale = static_cast<float>(LOWORD(wParam)) / USER_DEFAULT_SCREEN_DPI;
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            DispatchResize(window);
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wParam) != WA_INACTIVE) {
                WintabActivate(window);
            }
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            DispatchFocus(window, message == WM_SETFOCUS);
            return 0;
        case WT_PACKET:
            WintabHandlePackets(window);
            return 0;
        case WT_PROXIMITY:
            slot.tabletProximity = LOWORD(lParam) != 0;
            return 0;
        case kWmTabletQuerySystemGestureStatus:
            return kTabletDisableGestures;
        case WM_ERASEBKGND:
            // The renderer paints the whole client area; erasing first would only flicker.
            return 1;
        case WM_DESTROY: {
            DispatchDestroy(window);
            WintabClose(window);
            if (const auto res = GraphicsWindowTerminate(window); !res) {
                MIDORI_LOG_ERROR("GraphicsWindowTerminate failed: {}", ErrorMessage(res.error()));
            }
            UiWindowTerminate(window);
            slot = {};
            bool anyWindowLeft = false;
            for (const Win32Window& other : gPlatformState.windows) {
                if (other.hwnd) {
                    anyWindowLeft = true;
                }
            }
            if (!anyWindowLeft) {
                PostQuitMessage(0);
            }
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

Result<void> PlatformInit(PlatformInitInfo initInfo) noexcept {
    ZoneScopedN("PlatformInit");
    MIDORI_ASSERT_ALWAYS(initInfo.hInstance, "PlatformInit needs the HINSTANCE passed to wWinMain");
    gPlatformState.hInstance = initInfo.hInstance;
    gPlatformState.nShowCmd = initInfo.nShowCmd;

    // No CS_HREDRAW/CS_VREDRAW and no background brush: the renderer repaints the whole client area.
    const WNDCLASSEXW windowClass{
        .cbSize = sizeof(WNDCLASSEXW),
        .lpfnWndProc = WindowProc,
        .hInstance = initInfo.hInstance,
        .hIcon = LoadIconW(initInfo.hInstance, MAKEINTRESOURCEW(IDI_ICON1)),
        .hCursor = LoadCursorW(nullptr, IDC_ARROW),
        .lpszClassName = kWindowClassName,
    };
    gPlatformState.windowClass = RegisterClassExW(&windowClass);
    if (!gPlatformState.windowClass) {
        MIDORI_LOG_ERROR("RegisterClassExW failed: {}", GetLastError());
        return std::unexpected(Error::PlatformInitFailed);
    }

    WintabInit();
    return {};
}

void PlatformTerminate() noexcept {
    ZoneScopedN("PlatformTerminate");
    for (const Win32Window& slot : gPlatformState.windows) {
        if (slot.hwnd) {
            DestroyWindow(slot.hwnd);
        }
    }
    WintabTerminate();
    if (gPlatformState.windowClass) {
        UnregisterClassW(kWindowClassName, gPlatformState.hInstance);
    }
    gPlatformState = {};
}

Result<Window> WindowCreate(WindowCreateInfo createInfo) noexcept {
    ZoneScopedN("WindowCreate");
    Window window = kWindowInvalid;
    for (Window i = 0; i < kWindowMax; ++i) {
        if (!gPlatformState.windows[i].hwnd) {
            window = i;
            break;
        }
    }
    if (window == kWindowInvalid) {
        MIDORI_LOG_ERROR("WindowCreate: all {} windows are in use", kWindowMax);
        return std::unexpected(Error::WindowLimitReached);
    }

    wchar_t title[256];
    Utf8ToWide(createInfo.title, title);

    // Created hidden at the default place; the DPI of the monitor it lands on then sizes it. No
    // redirection bitmap: DirectComposition shows the swap chain, alpha included.
    Win32Window& slot = gPlatformState.windows[window];
    slot.state = WindowState{.id = window, .displayMode = WindowDisplayMode::Hidden};
    HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, MAKEINTATOM(gPlatformState.windowClass), title,
                                kWindowedStyle, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr,
                                nullptr, gPlatformState.hInstance, &window);
    if (!hwnd) {
        MIDORI_LOG_ERROR("CreateWindowExW failed: {}", GetLastError());
        slot = {};
        return std::unexpected(Error::WindowCreateFailed);
    }
    MIDORI_ASSERT(slot.hwnd == hwnd, "WM_NCCREATE did not claim window slot {}", window);

    const UINT dpi = GetDpiForWindow(hwnd);
    slot.state.dpiScale = static_cast<float>(dpi) / USER_DEFAULT_SCREEN_DPI;
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    int x = rect.left;
    int y = rect.top;
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (createInfo.x >= 0 && createInfo.y >= 0) {
        x = MulDiv(createInfo.x, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
        y = MulDiv(createInfo.y, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
    }
    if (createInfo.width >= 0 && createInfo.height >= 0) {
        RECT client{
            .left = 0,
            .top = 0,
            .right = MulDiv(createInfo.width, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
            .bottom = MulDiv(createInfo.height, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
        };
        AdjustWindowRectExForDpi(&client, kWindowedStyle, FALSE, 0, dpi);
        width = client.right - client.left;
        height = client.bottom - client.top;
    }
    SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);

    const auto ui = UiWindowInit(window);
    if (!ui) {
        DestroyWindow(hwnd);
        return std::unexpected(ui.error());
    }
    if (const auto res = GraphicsWindowInit(GraphicsWindowInitInfo{.ui = *ui, .window = window}); !res) {
        DestroyWindow(hwnd);
        return std::unexpected(res.error());
    }
    WintabOpen(window);

    if (createInfo.displayMode == WindowDisplayMode::Normal) {
        // Windows asks that the first ShowWindow use the entry point's nShowCmd.
        ShowWindow(hwnd, gPlatformState.nShowCmd);
    } else if (createInfo.displayMode != WindowDisplayMode::Hidden) {
        if (const auto shown = WindowSetDisplayMode(window, createInfo.displayMode); !shown) {
            return std::unexpected(shown.error());
        }
    }
    return window;
}

Result<void> WindowDestroy(Window window) noexcept {
    if (!WindowValid(window)) {
        return std::unexpected(Error::WindowInvalid);
    }
    // WM_DESTROY frees the slot.
    DestroyWindow(gPlatformState.windows[window].hwnd);
    return {};
}

Result<void*> WindowGetNativeHandle(Window window) noexcept {
    if (!WindowValid(window)) {
        return std::unexpected(Error::WindowInvalid);
    }
    return static_cast<void*>(gPlatformState.windows[window].hwnd);
}

Result<WindowState> WindowGetState(Window window) noexcept {
    if (!WindowValid(window)) {
        return std::unexpected(Error::WindowInvalid);
    }
    return gPlatformState.windows[window].state;
}

Result<void> WindowSetTitle(Window window, std::string_view title) noexcept {
    if (!WindowValid(window)) {
        return std::unexpected(Error::WindowInvalid);
    }
    wchar_t wide[256];
    Utf8ToWide(title, wide);
    SetWindowTextW(gPlatformState.windows[window].hwnd, wide);
    return {};
}

Result<void> WindowSetDisplayMode(Window window, WindowDisplayMode displayMode) noexcept {
    ZoneScopedN("WindowSetDisplayMode");
    if (!WindowValid(window)) {
        return std::unexpected(Error::WindowInvalid);
    }
    Win32Window& slot = gPlatformState.windows[window];
    const bool fullscreen = slot.state.displayMode == WindowDisplayMode::Fullscreened;

    if (displayMode == WindowDisplayMode::Fullscreened) {
        if (fullscreen) {
            return {};
        }
        slot.restore.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(slot.hwnd, &slot.restore);
        MONITORINFO monitor{.cbSize = sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(slot.hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        // Set first so the WM_SIZE below keeps it instead of reporting Normal.
        slot.state.displayMode = WindowDisplayMode::Fullscreened;
        SetWindowLongPtrW(slot.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(slot.hwnd, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        return {};
    }

    if (fullscreen) {
        slot.state.displayMode = WindowDisplayMode::Normal;
        SetWindowLongPtrW(slot.hwnd, GWL_STYLE, kWindowedStyle | WS_VISIBLE);
        SetWindowPlacement(slot.hwnd, &slot.restore);
        SetWindowPos(slot.hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    switch (displayMode) {
        case WindowDisplayMode::Normal:
            ShowWindow(slot.hwnd, SW_SHOWNORMAL);
            break;
        case WindowDisplayMode::Hidden:
            ShowWindow(slot.hwnd, SW_HIDE);
            break;
        case WindowDisplayMode::Minimized:
            ShowWindow(slot.hwnd, SW_MINIMIZE);
            break;
        case WindowDisplayMode::Maximized:
            ShowWindow(slot.hwnd, SW_MAXIMIZE);
            break;
        case WindowDisplayMode::Fullscreened:
            break;
    }
    return {};
}

void AssertAbort() noexcept {
    spdlog::default_logger_raw()->flush();
    BreakAndAbort();
}

} // namespace Midori

// Global new/delete, replaced so every C++ allocation (the STL's and spdlog's included) shows up in
// Tracy's memory view. Only the base forms are defined: the standard library's array, sized and
// nothrow-delete forms forward to them. The nothrow new forms are defined too, because the default
// ones call the throwing form, which aborts here instead of throwing. Aligned forms must use
// _aligned_malloc/_aligned_free: free() cannot release an aligned block.
//
// Performance: while a profiler is connected, every TracyAlloc/TracyFree (here and in Heap) takes
// Tracy's serial lock, so all threads' allocations are serialized through one mutex. If a capture
// shows freezes, stalls or contention around allocation-heavy code, look here first: compare a run
// with -DTRACY_ENABLE=OFF, or stop tracking the global new/delete and keep only Heap.
//
// Exit: the plain TracyAlloc/TracyFree assume Tracy's profiler is alive. Frees that run during
// static destruction, after the profiler has shut down, can touch a dead profiler. If the app
// crashes on exit inside Tracy, switch these to TracySecureAlloc/TracySecureFree, which check that
// the profiler is still available first (at the cost of one extra check per call).

void* operator new(std::size_t size) {
    void* ptr = std::malloc(size ? size : 1);
    if (!ptr) [[unlikely]] {
        OutputDebugStringA("Midori: out of memory in operator new\n");
        BreakAndAbort();
    }
    TracyAlloc(ptr, size);
    return ptr;
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    void* ptr = _aligned_malloc(size ? size : 1, static_cast<std::size_t>(alignment));
    if (!ptr) [[unlikely]] {
        OutputDebugStringA("Midori: out of memory in operator new\n");
        BreakAndAbort();
    }
    TracyAlloc(ptr, size);
    return ptr;
}

void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    void* ptr = std::malloc(size ? size : 1);
    if (ptr) {
        TracyAlloc(ptr, size);
    }
    return ptr;
}
void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    void* ptr = std::malloc(size ? size : 1);
    if (ptr) {
        TracyAlloc(ptr, size);
    }
    return ptr;
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t& /*tag*/) noexcept {
    void* ptr = _aligned_malloc(size ? size : 1, static_cast<std::size_t>(alignment));
    if (ptr) {
        TracyAlloc(ptr, size);
    }
    return ptr;
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t& /*tag*/) noexcept {
    void* ptr = _aligned_malloc(size ? size : 1, static_cast<std::size_t>(alignment));
    if (ptr) {
        TracyAlloc(ptr, size);
    }
    return ptr;
}

void operator delete(void* ptr) noexcept {
    if (ptr) {
        TracyFree(ptr);
        std::free(ptr);
    }
}
void operator delete(void* ptr, std::align_val_t /*alignment*/) noexcept {
    if (ptr) {
        TracyFree(ptr);
        _aligned_free(ptr);
    }
}
