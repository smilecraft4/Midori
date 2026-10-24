#pragma once

#include "midori/base.hpp"
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

namespace Midori {

// Defined by the backend (win32_platform.hpp): it carries the native entry-point arguments.
struct PlatformInitInfo;

// Platform and window calls belong to the thread that called PlatformInit.
[[nodiscard]] Result<void> PlatformInit(PlatformInitInfo initInfo) noexcept;
void PlatformTerminate() noexcept;

// One state at a time, not flags.
enum struct WindowDisplayMode : std::uint8_t {
    Normal,
    Hidden,
    Minimized,
    Maximized,
    Fullscreened,
};

// Shown while the pointer is over the client area. A cursor the system lacks shows as None.
enum struct WindowCursor : std::uint8_t {
    Arrow,
    None,
    Text,
    Hand,
    Move,
    ResizeNS,
    ResizeEW,
    ResizeNESW,
    ResizeNWSE,
    NotAllowed,
    Count_,
};

using Window = std::uint8_t;
constexpr Window kWindowMax = 8;
constexpr Window kWindowInvalid = std::numeric_limits<Window>::max();

// Any position is valid: a monitor left of or above the main one has negative coordinates.
constexpr int kWindowPosDefault = std::numeric_limits<int>::min();

// Position and size are in DIPs (96 DPI units), scaled to the monitor's DPI on creation so a window
// looks the same size on every screen. A position on no monitor falls back to the default.
struct WindowCreateInfo {
    std::string_view title;   // UTF-8
    int x{kWindowPosDefault}; // of the window's frame
    int y{kWindowPosDefault};
    int width{-1};            // `-1` for default width, otherwise the client area width
    int height{-1};           // `-1` for default height, otherwise the client area height
    WindowDisplayMode displayMode{};
};

// Client-area origin (screen coordinates) and size, in physical pixels. `dpiScale` is the window's
// DPI over 96: multiply DIPs by it to get pixels.
struct WindowState {
    int x{};
    int y{};
    int width{};
    int height{};
    float dpiScale{1.0f};
    Window id{kWindowInvalid};
    WindowDisplayMode displayMode{};
    bool hovered{}; // the pointer is over the client area
};

// The app quits once its last window is destroyed, whether by the user or by WindowDestroy.
[[nodiscard]] Result<Window> WindowCreate(WindowCreateInfo createInfo) noexcept;
[[nodiscard]] Result<void> WindowDestroy(Window window) noexcept;
[[nodiscard]] Result<void> WindowRender(Window window) noexcept;

[[nodiscard]] Result<WindowState> WindowGetState(Window window) noexcept;
// Where the window sits when restored, as WindowCreate takes it, and how it shows: maximized, fullscreen or else
// normal. The title is left empty.
[[nodiscard]] Result<WindowCreateInfo> WindowGetPlacement(Window window) noexcept;
[[nodiscard]] Result<void> WindowSetTitle(Window window, std::string_view title) noexcept;
// Fullscreened is borderless, covering the window's current monitor; leaving it restores the exact
// previous placement, maximized included when leaving for Normal.
[[nodiscard]] Result<void> WindowSetDisplayMode(Window window, WindowDisplayMode displayMode) noexcept;
// Takes effect at once when the pointer is over the window, else when it next enters.
[[nodiscard]] Result<void> WindowSetCursor(Window window, WindowCursor cursor) noexcept;

[[nodiscard]] Result<void*> WindowGetNativeHandle(Window window) noexcept;

// Where this user's settings of every app go, roaming with the user: %APPDATA% on Windows.
[[nodiscard]] Result<std::filesystem::path> PlatformUserSettingsPath() noexcept;

[[nodiscard]] inline std::string PathUtf8(const std::filesystem::path& path) noexcept {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

// Dialogs are modal to `window` (kWindowInvalid for none) and block in their own message loop, which keeps
// delivering events for every window.

enum struct DialogChoice : std::uint8_t {
    Yes,
    No,
    Cancel,
};

// Text is UTF-8. The instruction is the headline; the content, which may be empty, explains it.
struct DialogAskInfo {
    std::string_view title;
    std::string_view instruction;
    std::string_view content;
    std::string_view yes; // button labels; a Cancel button always follows them
    std::string_view no;
    Window window{kWindowInvalid};
};

// Yes is the default button. Cancel when the dialog cannot show, so nothing proceeds unasked.
[[nodiscard]] DialogChoice DialogAsk(const DialogAskInfo& info) noexcept;
void DialogError(Window window, std::string_view title, std::string_view instruction,
                 std::string_view content) noexcept;

struct DialogFileInfo {
    std::filesystem::path initial; // save: the suggested file
    std::string_view typeName;     // UTF-8, as the file type filter shows it
    std::string_view extension;    // without the dot
    Window window{kWindowInvalid};
};

// The chosen file, empty when the user cancels. Saving asks before replacing an existing file.
[[nodiscard]] Result<std::filesystem::path> DialogFileOpen(const DialogFileInfo& info) noexcept;
[[nodiscard]] Result<std::filesystem::path> DialogFileSave(const DialogFileInfo& info) noexcept;

} // namespace Midori
