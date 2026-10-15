#include "win32_platform.hpp"
#include "win32_graphics.hpp"

#include "midori/app.hpp"
#include "midori/base.hpp"
#include "midori/log.hpp"
#include "win32_resource.h"

#include <span>

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

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
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
            WintabClose(window);
            if (const auto res = GraphicsWindowTerminate(window); !res) {
                MIDORI_LOG_ERROR("GraphicsWindowTerminate failed: {}", ErrorMessage(res.error()));
            }
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

void PlatformDispatch(Event event) noexcept {
    if (const auto result = AppEvent(event); !result) {
        MIDORI_LOG_ERROR("AppEvent failed: {}", ErrorMessage(result.error()));
    }
}

Result<void> PlatformInit(PlatformInitInfo initInfo) noexcept {
    MIDORI_ASSERT_ALWAYS(initInfo.hInstance, "PlatformInit needs the HINSTANCE passed to wWinMain");
    gPlatformState.hInstance = initInfo.hInstance;
    gPlatformState.nShowCmd = initInfo.nShowCmd;

    // No CS_HREDRAW/CS_VREDRAW and no background brush: the renderer repaints the whole client area.
    const WNDCLASSEXW windowClass{
        .cbSize = sizeof(WNDCLASSEXW),
        .lpfnWndProc = WindowProc,
        .hInstance = initInfo.hInstance,
        .hIcon = static_cast<HICON>(LoadImageW(initInfo.hInstance, MAKEINTRESOURCEW(IDI_ICON1), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0)),
        .hCursor = LoadCursorW(nullptr, IDC_ARROW),
        .lpszClassName = kWindowClassName,
        .hIconSm = static_cast<HICON>(LoadImageW(initInfo.hInstance, MAKEINTRESOURCEW(IDI_ICON1), IMAGE_ICON,
                                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0)),
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

    if (const auto res = GraphicsWindowInit(GraphicsWindowInitInfo{.window = window}); !res) {
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

} // namespace Midori
