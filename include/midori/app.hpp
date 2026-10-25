#pragma once

#include "midori/action.hpp"
#include "midori/base.hpp"
#include "midori/canvas.hpp"
#include "midori/history.hpp"
#include "midori/input.hpp"
#include "midori/platform.hpp"
#include "midori/stroke.hpp"
#include "midori/text.hpp"
#include "midori/tile.hpp"
#include "midori/tool.hpp"
#include "midori/ui.hpp"
#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
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
    Viewport viewportStart; // where the current mode started from
    Viewport gestureStart;  // where Space + drag started, whatever modes it went through: one history step
    float pointerStartX;    // client-area physical pixels
    float pointerStartY;
    CanvasView view{kCanvasViewInvalid};
    NavigateMode mode{}; // None while not navigating
};

// Pointer distance from the center, in DIPs, for an opacity or hardness of 1.
constexpr float kBrushAdjustRangeDips = 150.0f;

enum struct BrushAdjustMode : std::uint8_t {
    None,
    Radius,
    Opacity,
    Hardness,
    LayerOpacity, // of `layer`, not the brush
};

// The one brush value, or layer opacity, being set by the pointer's distance from a fixed center, on `view`. The brush
// or layer holds the value live; canceling puts `startValue` back, keeping a layer opacity records one undo step.
struct BrushAdjustState {
    float centerX;              // client-area physical pixels
    float centerY;
    float distance;             // from the center at the last pointer move
    float value;                // before clamping and snapping
    float startValue;
    Layer layer{kLayerInvalid}; // LayerOpacity: the canvas's active layer when it started
    CanvasView view{kCanvasViewInvalid};
    Tool tool{};
    BrushAdjustMode mode{}; // None while not adjusting
};

enum struct TransformDrag : std::uint8_t {
    None,
    Move,        // inside the box
    Rotate,      // outside it
    Scale,       // a corner, or an edge along one axis
    Distort,     // a handle with Alt: its corner, or the two of its edge, alone; the box to any convex quad
};

// Around the center of the box under the selection's bounds as the transform began: distorted, scaled, turned
// clockwise, then moved.
struct TransformParams {
    double offsetX{}; // canvas units
    double offsetY{};
    double scaleX{1.0};
    double scaleY{1.0};
    double rotation{}; // radians
    StrokeTransform distort; // canvas units, before the rest: where the corners dragged alone put the box
};

// The selection's transform, a Transform stroke while it lasts. Every pointer move recomputes from the drag's start.
struct TransformState {
    TransformParams params;
    TransformParams start; // as the drag started
    double pointerStartX;  // canvas units
    double pointerStartY;
    TransformDrag drag{};
    std::int8_t handleX{}; // Scale and Distort: -1, 0 or 1 along the box's x axis, where the handle is
    std::int8_t handleY{};
    bool wasModified{};    // the canvas's before the transform, put back when canceled
    bool held{};           // begun by a drag with Ctrl held: applied once Ctrl is up and the drag over
};

// A transform's box as a view shows it: the transform in progress, or with Ctrl held the selection's, which a drag
// starts transforming.
struct TransformBox {
    StrokeTransform transform;
    SelectionBounds source;
    CanvasView view;
};

// Where a window shows the brush outline this frame, in physical pixels of its client area. AppWindowFrame sets it,
// the renderer draws it over the canvas, inverting what is below.
struct BrushOutlineState {
    float x;
    float y;
    float radius; // the current tool's brush, or what the stroke paints at right now
    float lineWidth;
    bool shown;
};

// Owned by the main thread. A canvas id indexes `canvases`, a canvas view id indexes `canvasViews`.
// Pointer and keyboard state is the devices', not any one view's.
struct AppState {
    std::array<WindowState, kWindowMax> windows;
    std::array<CanvasState, kCanvasMax> canvases;
    std::array<CanvasViewState, kCanvasViewMax> canvasViews;
    TileAtlasState atlas;
    TileTransfersState transfers;
    HistoryPoolState historyPool;
    NavigateState navigate;
    BrushAdjustState brushAdjust;
    std::array<BrushOutlineState, kWindowMax> brushOutlines; // indexed by window
    StrokeState stroke;
    TransformState transform;
    ToolsState tools;
    KeymapState keymap;                             // keybinds.json beside preferences.json, else kDefaultBindings
    std::vector<ActionId> recentActions;            // the most recent first, up to kRecentActionsMax
    std::vector<ActionRequest> actionRequests;      // picked by the UI, run by the next AppUpdate
    std::vector<std::filesystem::path> recentFiles; // opened or saved as, the most recent first, up to kRecentFilesMax
    std::string language;                           // the preference, a code of kLanguages or empty for the system's
    WindowCreateInfo windowPlacement; // the last window closed, or the first open at exit; its title unused
    CanvasFilter canvasFilter{CanvasFilter::Lanczos2}; // every view's
    // Shared by every window. Bumped at the start of a frame, right before the views report their visible
    // tile grid positions, so a tile with an older `visibleFrame` is shown nowhere.
    std::uint32_t frame;
    float pointerX; // last pointer position, client-area physical pixels of the window it moved in
    float pointerY;
    float pointerPressure;
    KeyMod keyMods; // as of the last key event
    bool spaceDown;
    bool modal;     // a dialog is up: its message loop still delivers events, which are dropped
};

extern AppState gAppState;

[[nodiscard]] Result<void> AppInit(AppInitInfo initInfo) noexcept;
[[nodiscard]] Result<void> AppEvent(Event event) noexcept;
// The renderer is about to draw this window: last chance to update what it shows. Runs inside the
// renderer's submit, so it must not create or destroy windows.
void AppWindowFrame(Window window) noexcept;
// Once per main loop iteration, outside any submit: runs the actions the UI picked.
[[nodiscard]] Result<void> AppUpdate() noexcept;
void AppTerminate() noexcept;

// The window shows a canvas, replacing its current one once the user has saved or discarded unsaved
// changes. Canceling any dialog is not an error. Failures are shown to the user, then returned.
[[nodiscard]] Result<void> AppCanvasNew(Window window) noexcept;
[[nodiscard]] Result<void> AppCanvasOpen(Window window) noexcept;
// A file of `recentFiles`; one that does not open leaves the list.
[[nodiscard]] Result<void> AppCanvasOpenRecent(Window window, std::size_t index) noexcept;
// Asks for a file when the canvas has none.
[[nodiscard]] Result<void> AppCanvasSave(Window window) noexcept;
[[nodiscard]] Result<void> AppCanvasSaveAs(Window window) noexcept;
// The user asked to close the window: destroys it once unsaved changes are saved or discarded.
[[nodiscard]] Result<void> AppWindowClose(Window window) noexcept;
// Closes every window the same way, stopping at the first the user keeps open.
[[nodiscard]] Result<void> AppQuit() noexcept;

// The window's canvas undoes or redoes one more stroke, once no stroke is on it and the tiles are loaded.
[[nodiscard]] Result<void> AppUndo(Window window) noexcept;
[[nodiscard]] Result<void> AppRedo(Window window) noexcept;

// On the window's canvas, each one undo step: deleting, hiding and blend mode act on the selected layers
// (LayersSelected), the others on the active one. What the canvas refuses (a merge, a stroke or undo steps in progress,
// or an op the layer does not allow) does nothing.
// A new layer, or folder, right above the active layer, which it becomes.
void AppLayerNew(Window window, bool folder) noexcept;
void AppLayerDelete(Window window) noexcept;
// Merges the active layer down, or the active folder into one layer; the result becomes active once done.
void AppLayerMerge(Window window) noexcept;
// Past the next layer or folder above, or below: into and out of folders one step at a time.
void AppLayerMove(Window window, bool up) noexcept;
// The next layer or folder above, or below, becomes active and the only one selected. No undo step.
void AppLayerSelect(Window window, bool up) noexcept;
void AppLayerToggleHidden(Window window) noexcept;
void AppLayerBlendMode(Window window, BlendMode blendMode) noexcept;
// The blend mode after the active layer's, or before it, wrapping around.
void AppLayerBlendModeCycle(Window window, bool next) noexcept;

// Why `action` with `arg` would do nothing in `window` right now, Text::Count_ when it would run. The search and the
// menus show it greyed out with the reason.
[[nodiscard]] Text ActionBlocked(Window window, ActionId action, std::uint32_t arg) noexcept;

// Borderless over the window's monitor, or back to how it was.
[[nodiscard]] Result<void> AppToggleFullscreen(Window window) noexcept;

[[nodiscard]] Result<void> AppPaintMode() noexcept;
[[nodiscard]] Result<void> AppEraseMode() noexcept;
[[nodiscard]] Result<void> AppSelectMode() noexcept;
// The window's selection and the active layer's pixels under it follow handles over the canvas until applied, as one
// undo step, or canceled.
[[nodiscard]] Result<void> AppTransformMode(Window window) noexcept;
void AppTransformEnd(bool apply) noexcept;
[[nodiscard]] bool AppTransforming() noexcept;
[[nodiscard]] std::optional<TransformBox> AppTransformBox(Window window) noexcept;

} // namespace Midori
