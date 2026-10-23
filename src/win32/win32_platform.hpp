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
#include <utility>

#include "midori/input.hpp"    // IWYU pragma: export
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
    HCTX tablet;           // Wintab context, null without a tablet
    DWORD tabletButtons;   // button bits of the last tablet packet, to detect press and release
    wchar_t highSurrogate; // first half of a UTF-16 pair from WM_CHAR, 0 when none is pending
    WindowCursor cursor;   // answered to WM_SETCURSOR over the client area
    bool tabletProximity;  // pen in range: mouse messages it generates are ignored
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
    std::array<HCURSOR, std::to_underlying(WindowCursor::Count_)> cursors; // shared system cursors, null for None
    int nShowCmd;
    ATOM windowClass;
};

extern PlatformState gPlatformState;

} // namespace Midori
