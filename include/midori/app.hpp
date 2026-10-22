#pragma once

#include "midori/base.hpp"
#include "midori/canvas.hpp"
#include "midori/input.hpp"
#include "midori/platform.hpp"
#include "midori/stroke.hpp"
#include "midori/tool.hpp"
#include "midori/ui.hpp"
#include <array>
#include <filesystem>
#include <span>
#include <vector>

namespace Midori {

struct AppInitInfo {
    std::span<const std::filesystem::path> files; // from the command line, each opened in its own window
};

enum struct NavigateMode : std::uint8_t {
    None,
    Pan,
    Zoom,
    Rotate,
};

// The one navigation drag in progress, on `view`. Every pointer move recomputes from the start.
struct NavigateState {
    Viewport viewportStart;
    float pointerStartX; // client-area physical pixels
    float pointerStartY;
    CanvasView view{kCanvasViewInvalid};
    NavigateMode mode{}; // None while not navigating
};

// Owned by the main thread. A canvas id indexes `canvases`, a canvas view id indexes `canvasViews`.
// Pointer and keyboard state is the devices', not any one view's.
struct AppState {
    std::array<WindowState, kWindowMax> windows;
    std::array<CanvasState, kCanvasMax> canvases;
    std::array<CanvasViewState, kCanvasViewMax> canvasViews;
    std::vector<TileAtlasCell> freeAtlasCells; // of the tile pool every canvas shares
    NavigateState navigate;
    StrokeState stroke;
    ToolsState tools;
    // Shared by every window. Bumped at the start of a frame, right before the views report their visible
    // tile grid positions, so a tile with an older `visibleFrame` is shown nowhere.
    std::uint32_t frame;
    float pointerX; // last pointer position, client-area physical pixels of the window it moved in
    float pointerY;
    float pointerPressure;
    float backgroundOpacity{0.5f}; // of the white behind the canvas, shared by every window
    KeyMod keyMods;                // as of the last key event
    Window commandWindow;
    UiCommand command;             // picked during `commandWindow`'s frame, run by the next AppUpdate
    bool spaceDown;
    bool modal;                    // a dialog is up: its message loop still delivers events, which are dropped
};

extern AppState gAppState;

[[nodiscard]] Result<void> AppInit(AppInitInfo initInfo) noexcept;
[[nodiscard]] Result<void> AppEvent(Event event) noexcept;
// The renderer is about to draw this window: last chance to update what it shows. Runs inside the
// renderer's submit, so it must not create or destroy windows.
void AppWindowFrame(Window window) noexcept;
// Once per main loop iteration, outside any submit: runs the UI command of the last frame.
[[nodiscard]] Result<void> AppUpdate() noexcept;
void AppTerminate() noexcept;

// The window shows a canvas, replacing its current one once the user has saved or discarded unsaved
// changes. Canceling any dialog is not an error. Failures are shown to the user, then returned.
[[nodiscard]] Result<void> AppCanvasNew(Window window) noexcept;
[[nodiscard]] Result<void> AppCanvasOpen(Window window) noexcept;
// Asks for a file when the canvas has none.
[[nodiscard]] Result<void> AppCanvasSave(Window window) noexcept;
[[nodiscard]] Result<void> AppCanvasSaveAs(Window window) noexcept;
// The user asked to close the window: destroys it once unsaved changes are saved or discarded.
[[nodiscard]] Result<void> AppWindowClose(Window window) noexcept;

[[nodiscard]] Result<void> AppPaintMode() noexcept;
[[nodiscard]] Result<void> AppEraseMode() noexcept;
[[nodiscard]] Result<void> AppSelectMode() noexcept;
[[nodiscard]] Result<void> AppTransformMode() noexcept;

} // namespace Midori
