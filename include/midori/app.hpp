#pragma once

#include "midori/base.hpp"
#include "midori/canvas.hpp"
#include "midori/input.hpp"
#include "midori/platform.hpp"
#include <array>
#include <string_view>

namespace Midori {

struct AppInitInfo {};

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
    NavigateState navigate;
    // Shared by every window. Bumped at the start of a frame, right before the views report their visible
    // tile grid positions, so a tile with an older `visibleFrame` is shown nowhere.
    std::uint32_t frame;
    float pointerX; // last pointer position, client-area physical pixels of the window it moved in
    float pointerY;
    KeyMod keyMods; // as of the last key event
    bool spaceDown;
};

extern AppState gAppState;

[[nodiscard]] Result<void> AppInit(AppInitInfo initInfo) noexcept;
[[nodiscard]] Result<void> AppEvent(Event event) noexcept;
// The renderer is about to draw this window: last chance to update what it shows. Runs inside the
// renderer's submit, so it must not create or destroy windows.
void AppWindowFrame(Window window) noexcept;
void AppTerminate() noexcept;

[[nodiscard]] Result<void> AppCanvasNew(Window window) noexcept;
[[nodiscard]] Result<void> AppCanvasOpen(Window window, std::string_view filename) noexcept;
[[nodiscard]] Result<void> AppCanvasSave(Canvas canvas) noexcept;
[[nodiscard]] Result<void> AppCanvasSaveAs(Canvas canvas, std::string_view filename) noexcept;

[[nodiscard]] Result<void> AppPaintMode() noexcept;
[[nodiscard]] Result<void> AppEraseMode() noexcept;
[[nodiscard]] Result<void> AppSelectMode() noexcept;
[[nodiscard]] Result<void> AppTransformMode() noexcept;

} // namespace Midori
