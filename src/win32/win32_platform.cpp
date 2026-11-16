#include "win32_platform.hpp"

// Packet layout: absolute positions, tip pressure, orientation (tilt), button bits and status (for
// the eraser bit). Buttons are absolute too: the current bit set, compared against the last packet.
#define PACKETDATA (PK_X | PK_Y | PK_NORMAL_PRESSURE | PK_ORIENTATION | PK_BUTTONS | PK_STATUS | PK_TIME)
#define PACKETMODE 0
#include <pktdef.h>

#include "win32_graphics.hpp"

#include "midori/app.hpp"
#include "midori/base.hpp"
#include "midori/input.hpp"
#include "midori/log.hpp"
#include "midori/ui.hpp"
#include "win32_resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <objbase.h>
#include <psapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
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

[[nodiscard]] std::wstring WideFromUtf8(std::string_view text) noexcept {
    std::wstring wide(static_cast<std::size_t>(
                          MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0)),
                      L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        static_cast<int>(wide.size()));
    return wide;
}

[[nodiscard]] HWND DialogOwner(Window window) noexcept {
    return WindowValid(window) ? gPlatformState.windows[window].hwnd : nullptr;
}

// The pressed button's id; IDCANCEL when the dialog cannot show. The first custom button is the default.
int DialogTask(Window window, std::string_view title, std::string_view instruction, std::string_view content,
               PCWSTR icon, std::span<const TASKDIALOG_BUTTON> buttons,
               TASKDIALOG_COMMON_BUTTON_FLAGS commonButtons) noexcept {
    ZoneScopedN("DialogTask");
    const std::wstring wideTitle = WideFromUtf8(title);
    const std::wstring wideInstruction = WideFromUtf8(instruction);
    const std::wstring wideContent = WideFromUtf8(content);
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = DialogOwner(window);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.dwCommonButtons = commonButtons;
    config.pszWindowTitle = wideTitle.c_str();
    config.pszMainIcon = icon;
    config.pszMainInstruction = wideInstruction.c_str();
    config.pszContent = wideContent.empty() ? nullptr : wideContent.c_str();
    config.cButtons = static_cast<UINT>(buttons.size());
    config.pButtons = buttons.data();
    config.nDefaultButton = buttons.empty() ? 0 : buttons.front().nButtonID;
    int pressed = IDCANCEL;
    if (const HRESULT hr = TaskDialogIndirect(&config, &pressed, nullptr, nullptr); FAILED(hr)) {
        MIDORI_LOG_ERROR("TaskDialogIndirect failed: {:#010x}", static_cast<std::uint32_t>(hr));
        return IDCANCEL;
    }
    return pressed;
}

// Releases the COM object it holds.
template<class T>
struct ComRef {
    T* ptr{};

    ComRef() = default;
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ~ComRef() {
        if (ptr) {
            ptr->Release();
        }
    }
};

// IFileDialog, which takes a check box under the file type. COM must be initialized.
[[nodiscard]] Result<std::filesystem::path> DialogFileShow(const DialogFileInfo& info, bool save) noexcept {
    std::wstring patterns;
    for (const auto part : std::views::split(info.extension, ';')) {
        patterns += (patterns.empty() ? L"*." : L";*.") + WideFromUtf8(std::string_view(part.begin(), part.end()));
    }
    const std::wstring typeName = WideFromUtf8(info.typeName) + L" (" + patterns + L")";
    const COMDLG_FILTERSPEC filter{.pszName = typeName.c_str(), .pszSpec = patterns.c_str()};
    const std::wstring extension = WideFromUtf8(info.extension.substr(0, info.extension.find(';')));
    const std::wstring check = WideFromUtf8(info.check);
    constexpr DWORD kCheckId = 1;

    ComRef<IFileDialog> dialog;
    FILEOPENDIALOGOPTIONS options{};
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog.ptr)))
        || FAILED(dialog.ptr->GetOptions(&options))
        || FAILED(dialog.ptr->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST
                                         | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST)))
        || FAILED(dialog.ptr->SetFileTypes(1, &filter)) || FAILED(dialog.ptr->SetDefaultExtension(extension.c_str()))) {
        MIDORI_LOG_ERROR("the file dialog could not be made");
        return std::unexpected(Error::DialogFailed);
    }
    if (!info.initial.empty()) {
        dialog.ptr->SetFileName(info.initial.filename().c_str());
        ComRef<IShellItem> folder;
        if (info.initial.has_parent_path()
            && SUCCEEDED(
                SHCreateItemFromParsingName(info.initial.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder.ptr)))) {
            dialog.ptr->SetFolder(folder.ptr);
        }
    }
    ComRef<IFileDialogCustomize> customize;
    if (info.checked && SUCCEEDED(dialog.ptr->QueryInterface(IID_PPV_ARGS(&customize.ptr)))) {
        customize.ptr->AddCheckButton(kCheckId, check.c_str(), *info.checked ? TRUE : FALSE);
    }
    if (const HRESULT shown = dialog.ptr->Show(DialogOwner(info.window)); FAILED(shown)) {
        if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            return std::filesystem::path();
        }
        MIDORI_LOG_ERROR("the file dialog failed: {:#010x}", static_cast<std::uint32_t>(shown));
        return std::unexpected(Error::DialogFailed);
    }
    if (BOOL checked = FALSE; customize.ptr && SUCCEEDED(customize.ptr->GetCheckButtonState(kCheckId, &checked))) {
        *info.checked = checked != FALSE;
    }
    ComRef<IShellItem> item;
    PWSTR raw = nullptr;
    if (FAILED(dialog.ptr->GetResult(&item.ptr)) || FAILED(item.ptr->GetDisplayName(SIGDN_FILESYSPATH, &raw))) {
        MIDORI_LOG_ERROR("the file dialog gave no file");
        return std::unexpected(Error::DialogFailed);
    }
    std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    return path;
}

[[nodiscard]] Result<std::filesystem::path> DialogFile(const DialogFileInfo& info, bool save) noexcept {
    ZoneScopedN("DialogFile");
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto path = DialogFileShow(info, save);
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    return path;
}

void DispatchResize(Window window) noexcept {
    ZoneScopedN("DispatchResize");
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

// The inverse of KeyFromMessage, with the side of each modifier. 0 for none.
[[nodiscard]] UINT VirtualKeyFromKey(Key key) noexcept {
    const auto offset = [key](Key first) {
        return static_cast<UINT>(std::to_underlying(key) - std::to_underlying(first));
    };
    if (key >= Key::A && key <= Key::Z) {
        return 'A' + offset(Key::A);
    }
    if (key >= Key::Num0 && key <= Key::Num9) {
        return '0' + offset(Key::Num0);
    }
    if (key >= Key::F1 && key <= Key::F24) {
        return VK_F1 + offset(Key::F1);
    }
    if (key >= Key::NumPad0 && key <= Key::NumPad9) {
        return VK_NUMPAD0 + offset(Key::NumPad0);
    }
    switch (key) {
        case Key::Escape:
            return VK_ESCAPE;
        case Key::Tab:
            return VK_TAB;
        case Key::CapsLock:
            return VK_CAPITAL;
        case Key::Backspace:
            return VK_BACK;
        case Key::Enter:
        case Key::NumPadEnter:
            return VK_RETURN;
        case Key::Space:
            return VK_SPACE;
        case Key::Insert:
            return VK_INSERT;
        case Key::Delete:
            return VK_DELETE;
        case Key::Home:
            return VK_HOME;
        case Key::End:
            return VK_END;
        case Key::PageUp:
            return VK_PRIOR;
        case Key::PageDown:
            return VK_NEXT;
        case Key::Left:
            return VK_LEFT;
        case Key::Right:
            return VK_RIGHT;
        case Key::Up:
            return VK_UP;
        case Key::Down:
            return VK_DOWN;
        case Key::Minus:
            return VK_OEM_MINUS;
        case Key::Equal:
            return VK_OEM_PLUS;
        case Key::LeftBracket:
            return VK_OEM_4;
        case Key::RightBracket:
            return VK_OEM_6;
        case Key::Backslash:
            return VK_OEM_5;
        case Key::Semicolon:
            return VK_OEM_1;
        case Key::Apostrophe:
            return VK_OEM_7;
        case Key::Grave:
            return VK_OEM_3;
        case Key::Comma:
            return VK_OEM_COMMA;
        case Key::Period:
            return VK_OEM_PERIOD;
        case Key::Slash:
            return VK_OEM_2;
        case Key::NumPadDecimal:
            return VK_DECIMAL;
        case Key::NumPadDivide:
            return VK_DIVIDE;
        case Key::NumPadMultiply:
            return VK_MULTIPLY;
        case Key::NumPadSubtract:
            return VK_SUBTRACT;
        case Key::NumPadAdd:
            return VK_ADD;
        case Key::NumLock:
            return VK_NUMLOCK;
        case Key::ScrollLock:
            return VK_SCROLL;
        case Key::PrintScreen:
            return VK_SNAPSHOT;
        case Key::Pause:
            return VK_PAUSE;
        case Key::Menu:
            return VK_APPS;
        case Key::LeftShift:
            return VK_LSHIFT;
        case Key::RightShift:
            return VK_RSHIFT;
        case Key::LeftCtrl:
            return VK_LCONTROL;
        case Key::RightCtrl:
            return VK_RCONTROL;
        case Key::LeftAlt:
            return VK_LMENU;
        case Key::RightAlt:
            return VK_RMENU;
        case Key::LeftSuper:
            return VK_LWIN;
        case Key::RightSuper:
            return VK_RWIN;
        default:
            return 0;
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
    ZoneScopedN("DispatchKey");
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
    ZoneScopedN("HandleButton");
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
    ZoneScopedN("DispatchMouseMove");
    Event event{};
    event.type = EventType::Pointer;
    event.pointer = PointerEvent{
        .time = static_cast<double>(static_cast<DWORD>(GetMessageTime())) / 1000.0,
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
    ZoneScopedN("DispatchText");
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
    ZoneScopedN("DispatchWheel");
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
    ZoneScopedN("DispatchFocus");
    Event event{};
    event.type = EventType::WindowFocus;
    event.windowFocus = WindowFocusEvent{.window = window, .focused = focused};
    PlatformDispatch(event);
}

void DispatchClose(Window window) noexcept {
    ZoneScopedN("DispatchClose");
    Event event{};
    event.type = EventType::WindowClose;
    event.windowClose = WindowCloseEvent{.window = window};
    PlatformDispatch(event);
}

void DispatchDestroy(Window window) noexcept {
    ZoneScopedN("DispatchDestroy");
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
        case WM_MOUSEMOVE: {
            // The pen moves the pointer too, so hovering is tracked before its messages are dropped.
            Win32Window& slot = gPlatformState.windows[window];
            if (!slot.state.hovered) {
                TRACKMOUSEEVENT track{.cbSize = sizeof(TRACKMOUSEEVENT), .dwFlags = TME_LEAVE, .hwndTrack = slot.hwnd};
                slot.state.hovered = TrackMouseEvent(&track) != 0;
            }
            if (!FromPen(window)) {
                DispatchMouseMove(window, wParam, lParam);
            }
            return true;
        }
        case WM_MOUSELEAVE:
            gPlatformState.windows[window].state.hovered = false;
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
    ZoneScopedN("WintabLoad");
    return reinterpret_cast<F>(GetProcAddress(dll, name));
}

void WintabTerminate() noexcept {
    ZoneScopedN("WintabTerminate");
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
    ZoneScopedN("WintabClose");
    Win32Window& slot = gPlatformState.windows[window];
    if (slot.tablet) {
        gPlatformState.wintab.close(slot.tablet);
        slot.tablet = nullptr;
    }
}

void WintabActivate(Window window) noexcept {
    ZoneScopedN("WintabActivate");
    // The focused window's context goes on top, so it is the one receiving packets.
    const Win32Window& slot = gPlatformState.windows[window];
    if (slot.tablet) {
        gPlatformState.wintab.overlap(slot.tablet, TRUE);
    }
}

void WintabDispatchButton(Window window, Device device, Button button, bool down) noexcept {
    ZoneScopedN("WintabDispatchButton");
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
            .time = static_cast<double>(packet.pkTime) / 1000.0,
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
            if (message == WM_KILLFOCUS) {
                if (GetCapture() == hwnd) {
                    ReleaseCapture();
                }
                slot.tabletButtons = 0;
                slot.tabletProximity = false;
                slot.highSurrogate = 0;
            }
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
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(gPlatformState.cursors[std::to_underlying(slot.cursor)]);
                return TRUE;
            }
            break;
        case WM_ERASEBKGND:
            // The renderer paints the whole client area; erasing first would only flicker.
            return 1;
        case WM_CLOSE:
            // The app destroys the window once nothing would be lost.
            DispatchClose(window);
            return 0;
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
    // Shared system cursors, never destroyed.
    const auto cursor = [](WindowCursor kind, LPCWSTR id) {
        gPlatformState.cursors[std::to_underlying(kind)] = LoadCursorW(nullptr, id);
    };
    cursor(WindowCursor::Arrow, IDC_ARROW);
    cursor(WindowCursor::Text, IDC_IBEAM);
    cursor(WindowCursor::Hand, IDC_HAND);
    cursor(WindowCursor::Move, IDC_SIZEALL);
    cursor(WindowCursor::ResizeNS, IDC_SIZENS);
    cursor(WindowCursor::ResizeEW, IDC_SIZEWE);
    cursor(WindowCursor::ResizeNESW, IDC_SIZENESW);
    cursor(WindowCursor::ResizeNWSE, IDC_SIZENWSE);
    cursor(WindowCursor::NotAllowed, IDC_NO);
    cursor(WindowCursor::Crosshair, IDC_CROSS);

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
    if (createInfo.x != kWindowPosDefault && createInfo.y != kWindowPosDefault) {
        const int placedX = MulDiv(createInfo.x, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
        const int placedY = MulDiv(createInfo.y, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
        // A monitor unplugged since leaves the default place.
        const RECT placed{.left = placedX, .top = placedY, .right = placedX + width, .bottom = placedY + height};
        if (MonitorFromRect(&placed, MONITOR_DEFAULTTONULL)) {
            x = placedX;
            y = placedY;
        }
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
    ZoneScopedN("WindowDestroy");
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

DialogChoice DialogAsk(const DialogAskInfo& info) noexcept {
    ZoneScopedN("DialogAsk");
    const std::wstring yes = WideFromUtf8(info.yes);
    const std::wstring no = WideFromUtf8(info.no);
    const std::array buttons{
        TASKDIALOG_BUTTON{.nButtonID = IDYES, .pszButtonText = yes.c_str()},
        TASKDIALOG_BUTTON{.nButtonID = IDNO, .pszButtonText = no.c_str()},
    };
    const std::span<const TASKDIALOG_BUTTON> shown = std::span(buttons).first(no.empty() ? 1 : 2);
    switch (DialogTask(info.window, info.title, info.instruction, info.content, nullptr, shown, TDCBF_CANCEL_BUTTON)) {
        case IDYES:
            return DialogChoice::Yes;
        case IDNO:
            return DialogChoice::No;
        default:
            return DialogChoice::Cancel;
    }
}

void DialogError(Window window, std::string_view title, std::string_view instruction,
                 std::string_view content) noexcept {
    ZoneScopedN("DialogError");
    DialogTask(window, title, instruction, content, TD_ERROR_ICON, {}, TDCBF_OK_BUTTON);
}

Result<std::filesystem::path> PlatformUserSettingsPath() noexcept {
    PWSTR raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(hr)) {
        MIDORI_LOG_ERROR("SHGetKnownFolderPath failed: {:#010x}", static_cast<std::uint32_t>(hr));
        CoTaskMemFree(raw);
        return std::unexpected(Error::PreferencesFailed);
    }
    std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    return path;
}

Result<std::filesystem::path> PlatformExecutableFolder() noexcept {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            MIDORI_LOG_ERROR("GetModuleFileNameW failed: {}", GetLastError());
            return std::unexpected(Error::Unknown);
        }
        if (length < path.size()) {
            path.resize(length);
            return std::filesystem::path(path).parent_path();
        }
        path.resize(path.size() * 2);
    }
}

std::string PlatformLanguage() noexcept {
    ULONG count = 0;
    std::array<wchar_t, 256> languages{};
    auto size = static_cast<ULONG>(languages.size());
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, languages.data(), &size) || count == 0
        || languages[0] == L'\0' || languages[1] == L'\0') {
        return "en";
    }
    const auto lower = [](wchar_t c) {
        return static_cast<char>(c >= L'A' && c <= L'Z' ? c - L'A' + L'a' : c);
    };
    return std::string{lower(languages[0]), lower(languages[1])};
}

std::string PlatformKeyName(Key key) noexcept {
    const std::string_view fallback = key < Key::Count_ ? kKeyNames[std::to_underlying(key)] : kKeyNames[0];
    const UINT vk = VirtualKeyFromKey(key);
    // The E0 prefix marks the extended keys: without it PageUp would read as the keypad's 9.
    UINT scancode = vk == 0 ? 0 : MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    if (key == Key::NumPadEnter) {
        scancode |= 0xE000;
    }
    if ((scancode & 0xFF) == 0) {
        return std::string(fallback);
    }
    auto lParam = static_cast<LONG>(scancode & 0xFF) << 16;
    if ((scancode & 0xFF00) == 0xE000) {
        lParam |= 1 << 24;
    }
    std::array<wchar_t, 64> name{};
    const int length = GetKeyNameTextW(lParam, name.data(), static_cast<int>(name.size()));
    if (length <= 0) {
        return std::string(fallback);
    }
    std::array<char, 256> utf8{};
    const int size = WideCharToMultiByte(CP_UTF8, 0, name.data(), length, utf8.data(), static_cast<int>(utf8.size()),
                                         nullptr, nullptr);
    if (size <= 0) {
        return std::string(fallback);
    }
    return {utf8.data(), static_cast<std::size_t>(size)};
}

Result<std::filesystem::path> PlatformFontPath(FontScript script) noexcept {
    PWSTR raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Fonts, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(hr)) {
        MIDORI_LOG_ERROR("SHGetKnownFolderPath failed: {:#010x}", static_cast<std::uint32_t>(hr));
        CoTaskMemFree(raw);
        return std::unexpected(Error::FontNotFound);
    }
    const std::filesystem::path folder(raw);
    CoTaskMemFree(raw);
    constexpr std::array<const wchar_t*, 1> kLatin = {L"segoeui.ttf"};
    constexpr std::array<const wchar_t*, 3> kJapanese = {L"YuGothM.ttc", L"meiryo.ttc", L"msgothic.ttc"};
    const std::span<const wchar_t* const> names =
        script == FontScript::Japanese ? std::span<const wchar_t* const>(kJapanese) : kLatin;
    for (const wchar_t* name : names) {
        std::error_code error;
        if (std::filesystem::path path = folder / name; std::filesystem::is_regular_file(path, error)) {
            return path;
        }
    }
    return std::unexpected(Error::FontNotFound);
}

Result<std::filesystem::path> DialogFileOpen(const DialogFileInfo& info) noexcept {
    ZoneScopedN("DialogFileOpen");
    return DialogFile(info, false);
}

Result<std::filesystem::path> DialogFileSave(const DialogFileInfo& info) noexcept {
    ZoneScopedN("DialogFileSave");
    return DialogFile(info, true);
}

Result<WindowState> WindowGetState(Window window) noexcept {
    if (!WindowValid(window)) {
        return std::unexpected(Error::WindowInvalid);
    }
    return gPlatformState.windows[window].state;
}

Result<WindowCreateInfo> WindowGetPlacement(Window window) noexcept {
    if (!WindowValid(window)) {
        return std::unexpected(Error::WindowInvalid);
    }
    const Win32Window& slot = gPlatformState.windows[window];
    // Fullscreen keeps the placement it left.
    WINDOWPLACEMENT placement = slot.restore;
    const bool fullscreen = slot.state.displayMode == WindowDisplayMode::Fullscreened;
    if (!fullscreen) {
        placement.length = sizeof(WINDOWPLACEMENT);
        if (!GetWindowPlacement(slot.hwnd, &placement)) {
            MIDORI_LOG_ERROR("GetWindowPlacement failed: {}", GetLastError());
            return std::unexpected(Error::WindowInvalid);
        }
    }
    WindowDisplayMode displayMode = WindowDisplayMode::Normal;
    if (fullscreen) {
        displayMode = WindowDisplayMode::Fullscreened;
    } else if (placement.showCmd == SW_SHOWMAXIMIZED
               || (placement.showCmd == SW_SHOWMINIMIZED && (placement.flags & WPF_RESTORETOMAXIMIZED) != 0)) {
        displayMode = WindowDisplayMode::Maximized;
    }
    // In workspace coordinates, from the work area of its monitor, which a taskbar on the left or top moves.
    RECT rect = placement.rcNormalPosition;
    MONITORINFO monitor{.cbSize = sizeof(MONITORINFO)};
    if (GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &monitor)) {
        OffsetRect(&rect, monitor.rcWork.left - monitor.rcMonitor.left, monitor.rcWork.top - monitor.rcMonitor.top);
    }
    // WindowCreate takes the frame's position but the client area's size.
    const UINT dpi = GetDpiForWindow(slot.hwnd);
    RECT frame{};
    AdjustWindowRectExForDpi(&frame, kWindowedStyle, FALSE, 0, dpi);
    const auto dips = [dpi](LONG pixels) {
        return MulDiv(pixels, USER_DEFAULT_SCREEN_DPI, static_cast<int>(dpi));
    };
    return WindowCreateInfo{
        .title = {},
        .x = dips(rect.left),
        .y = dips(rect.top),
        .width = dips((rect.right - rect.left) - (frame.right - frame.left)),
        .height = dips((rect.bottom - rect.top) - (frame.bottom - frame.top)),
        .displayMode = displayMode,
    };
}

Result<void> WindowSetCursor(Window window, WindowCursor cursor) noexcept {
    if (!WindowValid(window)) {
        return std::unexpected(Error::WindowInvalid);
    }
    Win32Window& slot = gPlatformState.windows[window];
    if (slot.cursor == cursor) {
        return {};
    }
    slot.cursor = cursor;
    if (slot.state.hovered) {
        SetCursor(gPlatformState.cursors[std::to_underlying(cursor)]);
    }
    return {};
}

Result<void> WindowSetTitle(Window window, std::string_view title) noexcept {
    ZoneScopedN("WindowSetTitle");
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
        // The placement brings back maximized too, which showing it normal would undo.
        if (displayMode == WindowDisplayMode::Normal) {
            return {};
        }
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

namespace {

// The first frame `decoder` reads, converted by WIC to `format`, 32 bits a pixel.
[[nodiscard]] Result<PlatformImage> ImageDecode(IWICImagingFactory* factory, IWICBitmapDecoder* decoder,
                                                const WICPixelFormatGUID& format) noexcept {
    ComRef<IWICBitmapFrameDecode> frame;
    ComRef<IWICFormatConverter> converter;
    UINT width = 0;
    UINT height = 0;
    if (FAILED(decoder->GetFrame(0, &frame.ptr)) || FAILED(frame.ptr->GetSize(&width, &height)) || width == 0
        || height == 0 || width > kPlatformImageSideMax || height > kPlatformImageSideMax
        || FAILED(factory->CreateFormatConverter(&converter.ptr))
        || FAILED(converter.ptr->Initialize(frame.ptr, format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                            WICBitmapPaletteTypeCustom))) {
        MIDORI_LOG_WARN("image of {}x{} not decoded", width, height);
        return std::unexpected(Error::ImageDecodeFailed);
    }
    PlatformImage image{
        .pixels = std::vector<std::byte>(std::size_t{width} * height * 4),
        .width = width,
        .height = height,
    };
    if (const HRESULT hr = converter.ptr->CopyPixels(nullptr, width * 4, static_cast<UINT>(image.pixels.size()),
                                                     reinterpret_cast<BYTE*>(image.pixels.data()));
        FAILED(hr)) {
        MIDORI_LOG_WARN("IWICFormatConverter::CopyPixels failed: {:#010x}", static_cast<std::uint32_t>(hr));
        return std::unexpected(Error::ImageDecodeFailed);
    }
    return image;
}

// An encoded image in memory, or else the file at `path`. COM must be initialized.
[[nodiscard]] Result<PlatformImage> ImageDecode(std::span<std::byte> data, const std::wstring& path,
                                                const WICPixelFormatGUID& format) noexcept {
    ComRef<IWICImagingFactory> factory;
    ComRef<IWICStream> stream;
    ComRef<IWICBitmapDecoder> decoder;
    if (const HRESULT hr =
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory.ptr));
        FAILED(hr)) {
        MIDORI_LOG_ERROR("CoCreateInstance(WICImagingFactory) failed: {:#010x}", static_cast<std::uint32_t>(hr));
        return std::unexpected(Error::ImageDecodeFailed);
    }
    HRESULT hr = S_OK;
    if (!path.empty()) {
        hr = factory.ptr->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                    &decoder.ptr);
    } else if (SUCCEEDED(hr = factory.ptr->CreateStream(&stream.ptr))
               && SUCCEEDED(hr = stream.ptr->InitializeFromMemory(reinterpret_cast<BYTE*>(data.data()),
                                                                  static_cast<DWORD>(data.size())))) {
        hr = factory.ptr->CreateDecoderFromStream(stream.ptr, nullptr, WICDecodeMetadataCacheOnDemand, &decoder.ptr);
    }
    if (FAILED(hr)) {
        MIDORI_LOG_WARN("no WIC decoder for the image: {:#010x}", static_cast<std::uint32_t>(hr));
        return std::unexpected(Error::ImageDecodeFailed);
    }
    return ImageDecode(factory.ptr, decoder.ptr, format);
}

// A clipboard bitmap is a BMP file without its file header: put back, WIC decodes it like one. A 32-bit bitmap
// without an alpha mask comes out opaque.
[[nodiscard]] bool DibToBmp(std::vector<std::byte>& data) noexcept {
    BITMAPINFOHEADER header{};
    if (data.size() < sizeof(header)) {
        return false;
    }
    std::memcpy(&header, data.data(), sizeof(header));
    const std::size_t colors =
        header.biClrUsed != 0 ? header.biClrUsed : (header.biBitCount <= 8 ? std::size_t{1} << header.biBitCount : 0);
    const std::size_t masks =
        header.biSize == sizeof(BITMAPINFOHEADER) && header.biCompression == BI_BITFIELDS ? 3 * sizeof(DWORD) : 0;
    const BITMAPFILEHEADER file{
        .bfType = 0x4D42, // "BM"
        .bfSize = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + data.size()),
        .bfReserved1 = 0,
        .bfReserved2 = 0,
        .bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + header.biSize + masks + (colors * sizeof(RGBQUAD))),
    };
    const auto* bytes = reinterpret_cast<const std::byte*>(&file);
    data.insert(data.begin(), bytes, bytes + sizeof(file));
    return true;
}

// Premultiplied RGBA to straight BGRA, the order and alpha PNG and the clipboard bitmap take.
[[nodiscard]] std::vector<std::byte> BgraStraight(const PlatformImage& image) noexcept {
    std::vector<std::byte> bgra(image.pixels.size());
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        const auto alpha = std::to_integer<unsigned>(image.pixels[i + 3]);
        const auto straight = [&](std::size_t channel) {
            const unsigned value = std::to_integer<unsigned>(image.pixels[i + channel]);
            return static_cast<std::byte>(alpha == 0 ? 0u : std::min(255u, ((value * 255u) + (alpha / 2)) / alpha));
        };
        bgra[i] = straight(2);
        bgra[i + 1] = straight(1);
        bgra[i + 2] = straight(0);
        bgra[i + 3] = image.pixels[i + 3];
    }
    return bgra;
}

// Straight BGRA as PNG into movable memory, which the caller or the clipboard then owns; `size` is the PNG's, the
// memory may be larger. Null when WIC fails. COM must be initialized.
[[nodiscard]] HGLOBAL PngEncode(std::span<std::byte> bgra, UINT width, UINT height, std::size_t& size) noexcept {
    ComRef<IWICImagingFactory> factory;
    ComRef<IStream> stream;
    ComRef<IWICBitmapEncoder> encoder;
    ComRef<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory.ptr)))
        // Not deleted with the stream: the clipboard takes the memory.
        || FAILED(CreateStreamOnHGlobal(nullptr, FALSE, &stream.ptr))
        || FAILED(factory.ptr->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder.ptr))
        || FAILED(encoder.ptr->Initialize(stream.ptr, WICBitmapEncoderNoCache))
        || FAILED(encoder.ptr->CreateNewFrame(&frame.ptr, nullptr)) || FAILED(frame.ptr->Initialize(nullptr))
        || FAILED(frame.ptr->SetSize(width, height)) || FAILED(frame.ptr->SetPixelFormat(&format))
        || format != GUID_WICPixelFormat32bppBGRA
        || FAILED(frame.ptr->WritePixels(height, width * 4, static_cast<UINT>(bgra.size()),
                                         reinterpret_cast<BYTE*>(bgra.data())))
        || FAILED(frame.ptr->Commit()) || FAILED(encoder.ptr->Commit())) {
        MIDORI_LOG_WARN("PNG of {}x{} not encoded", width, height);
        return nullptr;
    }
    ULARGE_INTEGER end{};
    HGLOBAL memory = nullptr;
    if (FAILED(stream.ptr->Seek(LARGE_INTEGER{}, STREAM_SEEK_CUR, &end))
        || FAILED(GetHGlobalFromStream(stream.ptr, &memory))) {
        return nullptr;
    }
    size = static_cast<std::size_t>(end.QuadPart);
    return memory;
}

// The same pixels as a bottom-up bitmap with an alpha mask, the layout most apps read.
[[nodiscard]] HGLOBAL DibV5Make(std::span<const std::byte> bgra, UINT width, UINT height) noexcept {
    const std::size_t row = std::size_t{width} * 4;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + bgra.size());
    auto* bytes = memory ? static_cast<std::byte*>(GlobalLock(memory)) : nullptr;
    if (!bytes) {
        if (memory) {
            GlobalFree(memory);
        }
        return nullptr;
    }
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(BITMAPV5HEADER);
    header.bV5Width = static_cast<LONG>(width);
    header.bV5Height = static_cast<LONG>(height);
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5SizeImage = static_cast<DWORD>(bgra.size());
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    header.bV5CSType = LCS_sRGB;
    header.bV5Intent = LCS_GM_IMAGES;
    std::memcpy(bytes, &header, sizeof(header));
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(bytes + sizeof(header) + (std::size_t{height - 1 - y} * row), bgra.data() + (std::size_t{y} * row),
                    row);
    }
    GlobalUnlock(memory);
    return memory;
}

} // namespace

bool PlatformClipboardHasImage() noexcept {
    return IsClipboardFormatAvailable(RegisterClipboardFormatW(L"PNG")) || IsClipboardFormatAvailable(CF_DIBV5)
        || IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_HDROP);
}

// PNG first, which keeps the alpha browsers and screenshots put there, then a bitmap, then a copied file. Copied out
// of the clipboard, which is not held open while decoding.
Result<PlatformImage> PlatformClipboardImage() noexcept {
    ZoneScopedN("PlatformClipboardImage");
    if (!OpenClipboard(nullptr)) {
        MIDORI_LOG_ERROR("OpenClipboard failed: {}", GetLastError());
        return std::unexpected(Error::ClipboardFailed);
    }
    std::vector<std::byte> data;
    std::wstring path;
    const auto copy = [&data](UINT format) {
        HANDLE handle = IsClipboardFormatAvailable(format) ? GetClipboardData(format) : nullptr;
        const void* memory = handle ? GlobalLock(handle) : nullptr;
        if (!memory) {
            return false;
        }
        const auto* bytes = static_cast<const std::byte*>(memory);
        data.assign(bytes, bytes + GlobalSize(handle));
        GlobalUnlock(handle);
        return true;
    };
    bool dib = false;
    if (!copy(RegisterClipboardFormatW(L"PNG"))) {
        dib = copy(CF_DIBV5) || copy(CF_DIB);
        if (!dib && IsClipboardFormatAvailable(CF_HDROP)) {
            if (auto* drop = static_cast<HDROP>(GetClipboardData(CF_HDROP))) {
                const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
                path.resize(length);
                DragQueryFileW(drop, 0, path.data(), length + 1);
            }
        }
    }
    CloseClipboard();
    if ((data.empty() && path.empty()) || (dib && !DibToBmp(data))) {
        return std::unexpected(Error::ClipboardFailed);
    }
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto image = ImageDecode(data, path, GUID_WICPixelFormat32bppPRGBA);
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    return image;
}

Result<PlatformImage> PlatformImageRead(const std::filesystem::path& path) noexcept {
    ZoneScopedN("PlatformImageRead");
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto image = ImageDecode({}, path.wstring(), GUID_WICPixelFormat32bppRGBA);
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    return image;
}

Result<void> PlatformImageWrite(const std::filesystem::path& path, const PlatformImage& image) noexcept {
    ZoneScopedN("PlatformImageWrite");
    std::vector<std::byte> bgra = BgraStraight(image);
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::size_t size = 0;
    HGLOBAL png = PngEncode(bgra, image.width, image.height, size);
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    const void* bytes = png ? GlobalLock(png) : nullptr;
    if (!bytes) {
        if (png) {
            GlobalFree(png);
        }
        return std::unexpected(Error::ImageEncodeFailed);
    }
    std::filesystem::path temp = path;
    temp += L".tmp";
    bool written = false;
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
        written = static_cast<bool>(file);
    }
    GlobalUnlock(png);
    GlobalFree(png);
    std::error_code ec;
    if (written) {
        std::filesystem::rename(temp, path, ec);
    }
    if (!written || ec) {
        MIDORI_LOG_ERROR("writing {} failed", PathUtf8(path));
        std::filesystem::remove(temp, ec);
        return std::unexpected(Error::ImageEncodeFailed);
    }
    return {};
}

std::uint32_t PlatformClipboardSequence() noexcept {
    return GetClipboardSequenceNumber();
}

Result<void> PlatformClipboardSetImage(Window window, const PlatformImage& image) noexcept {
    ZoneScopedN("PlatformClipboardSetImage");
    if (!WindowValid(window)) {
        return std::unexpected(Error::WindowInvalid);
    }
    std::vector<std::byte> bgra = BgraStraight(image);
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::size_t pngSize = 0;
    HGLOBAL png = PngEncode(bgra, image.width, image.height, pngSize);
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    HGLOBAL dib = DibV5Make(bgra, image.width, image.height);
    // An owner window, or EmptyClipboard leaves none and SetClipboardData fails.
    if (!OpenClipboard(gPlatformState.windows[window].hwnd)) {
        MIDORI_LOG_ERROR("OpenClipboard failed: {}", GetLastError());
        for (HGLOBAL memory : {png, dib}) {
            if (memory) {
                GlobalFree(memory);
            }
        }
        return std::unexpected(Error::ClipboardFailed);
    }
    EmptyClipboard();
    bool set = false;
    for (const auto [format, memory] :
         {std::pair{RegisterClipboardFormatW(L"PNG"), png}, std::pair{UINT{CF_DIBV5}, dib}}) {
        if (!memory) {
            continue;
        }
        if (SetClipboardData(format, memory)) {
            set = true;
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
    return set ? Result<void>{} : std::unexpected(Error::ClipboardFailed);
}

PlatformMemory PlatformMemoryGet() noexcept {
    PROCESS_MEMORY_COUNTERS_EX counters{.cb = sizeof(PROCESS_MEMORY_COUNTERS_EX)};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) {
        return {};
    }
    return PlatformMemory{
        .privateBytes = counters.PrivateUsage,
        .workingSet = counters.WorkingSetSize,
        .peakWorkingSet = counters.PeakWorkingSetSize,
    };
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
