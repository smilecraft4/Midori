#pragma once

// WINVER, _WIN32_WINNT, WIN32_LEAN_AND_MEAN, NOMINMAX and UNICODE come from CMake so that every
// translation unit sees <windows.h> the same way.
#include <windows.h>

// Wintab types and constants only: its functions are loaded from Wintab32.dll at runtime, and its
// fixed-point macros (INT, FRAC, ROUND...) would clash with ordinary names. Its 32-bit definitions
// are keyed on WIN32, which only CMake's default flags define.
#ifndef WIN32
#define WIN32
#endif
#define NOFIX32
#define NOWTFUNCTIONS
#include <wintab.h>

#include <array>

#include "midori/input.hpp"
#include "midori/platform.hpp" // IWYU pragma: export

namespace Midori {

struct PlatformInitInfo {
    HINSTANCE hInstance;
    int nShowCmd;
};

// One window slot; a null `hwnd` marks it free.
struct Win32Window {
    WINDOWPLACEMENT restore; // placement before entering fullscreen
    WindowState state;
    HWND hwnd;
    HCTX tablet;          // Wintab context, null without a tablet
    DWORD tabletButtons;  // button bits of the last tablet packet, to detect press and release
    bool tabletProximity; // pen in range: mouse messages it generates are ignored
};

// Wintab32.dll entry points, loaded at PlatformInit. All null when no tablet driver is installed.
struct Wintab {
    HMODULE dll;
    UINT(WINAPI* info)(UINT, UINT, LPVOID);
    HCTX(WINAPI* open)(HWND, LPLOGCONTEXTW, BOOL);
    BOOL(WINAPI* close)(HCTX);
    int(WINAPI* packetsGet)(HCTX, int, LPVOID);
    BOOL(WINAPI* overlap)(HCTX, BOOL);
    BOOL(WINAPI* queueSizeSet)(HCTX, int);
    LOGCONTEXTW context; // opened once per window
    // Packets come in tablet units over [0, tabletWidth] x [0, tabletHeight]; the driver maps that
    // area to this screen rectangle.
    float tabletWidth;
    float tabletHeight;
    float screenX;
    float screenY;
    float screenWidth;
    float screenHeight;
    float maxPressure;
    float maxAzimuth;  // 0 when the pen reports no orientation
    float maxAltitude; // 0 when the pen reports no orientation
};

// The win32 backend's state. Owned by the main thread (the one that ran wWinMain); nothing else may
// touch it. A window id indexes `windows`.
struct PlatformState {
    HINSTANCE hInstance;
    std::array<Win32Window, kWindowMax> windows;
    Wintab wintab;
    int nShowCmd;
    ATOM windowClass;
};

extern PlatformState gPlatformState;

// Hands an event to AppEvent and logs a failure (win32_platform.cpp).
void PlatformDispatch(Event event) noexcept;

// Keyboard and mouse messages. Returns true when the message was consumed (win32_input.cpp).
[[nodiscard]] bool InputHandleMessage(Window window, UINT message, WPARAM wParam, LPARAM lParam) noexcept;

// Wintab pen input (win32_wintab.cpp). Every call is a no-op without a tablet.
void WintabInit() noexcept;
void WintabTerminate() noexcept;
void WintabOpen(Window window) noexcept;
void WintabClose(Window window) noexcept;
void WintabActivate(Window window) noexcept;
void WintabHandlePackets(Window window) noexcept;

} // namespace Midori
