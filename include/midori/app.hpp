#pragma once

#include "midori/action.hpp"
#include "midori/base.hpp"
#include "midori/canvas.hpp"
#include "midori/history.hpp"
#include "midori/input.hpp"
#include "midori/platform.hpp"
#include "midori/stroke.hpp"
#include "midori/tag.hpp"
#include "midori/text.hpp"
#include "midori/tile.hpp"
#include "midori/tool.hpp"
#include "midori/ui.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
    Button button{};     // Left: with view.navigate held, the modes from the modifiers; Right: a pan alone
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

// Among `params`, or -1: what a brush adjust sets, the param named size for Radius, opacity for Opacity and hardness
// for Hardness.
inline int BrushAdjustParam(std::span<const MdslParam> params, BrushAdjustMode mode) noexcept {
    switch (mode) {
        case BrushAdjustMode::Radius:
            return BrushParamFind(params, "size");
        case BrushAdjustMode::Opacity:
            return BrushParamFind(params, "opacity");
        case BrushAdjustMode::Hardness:
            return BrushParamFind(params, "hardness");
        case BrushAdjustMode::LayerOpacity:
        case BrushAdjustMode::None:
            break;
    }
    return -1;
}

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
    BrushAdjustMode mode{};     // None while not adjusting
};

enum struct TransformDrag : std::uint8_t {
    None,
    Move,    // inside the box
    Rotate,  // outside it
    Scale,   // a corner, or an edge along one axis
    Distort, // a handle with Alt: its corner, or the two of its edge, alone; the box to any convex quad
};

// Around the center of the box under the selection's bounds as the transform began: distorted, scaled, turned
// clockwise, then moved.
struct TransformParams {
    double offsetX{}; // canvas units
    double offsetY{};
    double scaleX{1.0};
    double scaleY{1.0};
    double rotation{};       // radians
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

// An image pasted into a new layer: uploaded a tile per free upload slot over the updates, then selected, then
// transformed. See PasteUpdate.
struct PasteState {
    std::vector<std::byte> pixels;      // RGBA8 premultiplied, `width` x `height`, rows top to bottom
    std::vector<TileGridPos> remaining; // grid positions not uploaded yet
    std::int64_t x;                     // the canvas texel of its top left pixel
    std::int64_t y;
    std::uint32_t width;
    std::uint32_t height;
    Window window{kWindowInvalid};
    Canvas canvas{kCanvasInvalid}; // kCanvasInvalid: no paste
    Layer layer{kLayerInvalid};
    bool selected;                 // the selection was made around it: the transform comes next
};

// Where the last copy came from, so pasting it puts it back there while the clipboard still holds it.
struct CopyState {
    std::int64_t x; // the canvas texel of its top left pixel
    std::int64_t y;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t sequence; // PlatformClipboardSequence right after it was put there
    bool copied;
};

// A window's client area split in two, again and again: a binary tree whose leaves are panes, each showing the active
// one of its tabs (the canvas views whose `pane` it is).
constexpr std::uint8_t kLayoutNodesMax = 15; // 8 panes and the 7 splits between them

enum struct LayoutSplit : std::uint8_t {
    None,    // a leaf: a pane
    Columns, // `first` left of `second`
    Rows,    // `first` above `second`
};

// A pane's tab strip, shown with two tabs or more, in DIPs. The line between the two sides of a split is one physical
// pixel; its handle reaches kLayoutSplitterGrabDips over both sides.
constexpr float kLayoutTabStripDips = 26.0f;
constexpr int kLayoutSplitterPixels = 1;
constexpr float kLayoutSplitterGrabDips = 3.0f;

struct LayoutNode {
    CanvasViewRect rect;                   // where it lies, its tab strip included; rebuilt with its views' rects
    float share{0.5f};                     // a split: `first`'s part of its width or height, the rest `second`'s
    std::uint8_t first{};                  // a split: its two nodes
    std::uint8_t second{};
    CanvasView active{kCanvasViewInvalid}; // a pane: its tab shown, kCanvasViewInvalid with no tab
    LayoutSplit split{};
};

// nodes[0] is the root, nodes[0, count) in use. A pane with no tab left goes, its sibling taking the split's place.
struct WindowLayout {
    std::array<LayoutNode, kLayoutNodesMax> nodes{};
    CanvasViewRect area{};  // what the panes divide: the UI's dock space middle as of its last frame; empty: all of it
    std::uint8_t count{1};
    std::uint8_t focused{}; // the pane the keyboard, menus and panels act on: WindowCanvasView is its active tab
};

// A left drag of a Color tool: the current color slot takes the color shown under the pointer, sampled once per update
// while it moves.
struct EyedropperState {
    double x;                            // canvas units, the pointer's last
    double y;
    CanvasView view{kCanvasViewInvalid}; // kCanvasViewInvalid: no drag
    bool moved;                          // since the last sample
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

// A PNG being exported over the updates: the canvas's root composites at `remaining` are wanted a few at a time, and
// each one current is copied into `pixels`; the file is written once none remain.
struct ExportState {
    std::filesystem::path path;
    std::vector<std::byte> pixels;  // RGBA8 premultiplied, width x height, rows top to bottom
    std::vector<TileKey> remaining; // grid positions (level-0 keys, layer bits zero) not copied yet, row by row
    std::int64_t x;                 // the canvas texel of its top left pixel
    std::int64_t y;
    std::uint32_t width;
    std::uint32_t height;
    Color::Srgb background;        // under everything, as the canvas shows it; alpha 0 leaves the image transparent
    Canvas canvas{kCanvasInvalid}; // kCanvasInvalid while not exporting
    Window window{kWindowInvalid}; // the one it was asked from, which shows its errors
    bool masked;                   // times the selection's coverage; else cropped to what is drawn
};

enum struct BenchmarkPhase : std::uint8_t {
    None,
    Paint,    // strokes over a generated scene, until every tile is composited
    Navigate, // a scripted zoom and pan across it, frame by frame
    Open,     // saved, closed and opened again, until the whole view shows
    Export,   // the whole scene as a PNG
};

struct BenchmarkResult {
    std::string_view name;
    double milliseconds;
};

// debug.benchmark, run over the updates in a tab of its own, closed at the end. Each phase waits for the app to be idle
// before the next starts; the results go to benchmarks.csv beside preferences.json and the statistics window.
struct BenchmarkState {
    std::vector<BenchmarkResult> results;            // the last run's, in phase order
    std::filesystem::path file;                      // the scene saved, in the temp directory
    std::chrono::steady_clock::time_point phaseStart;
    std::chrono::steady_clock::time_point frameTime; // the last update's while navigating
    double frameMax;                                 // milliseconds between two updates while navigating
    std::uint32_t step;                              // strokes painted, or frames navigated
    std::uint32_t idleUpdates;                       // in a row with nothing left to do, see BenchmarkSettled
    Canvas canvas{kCanvasInvalid};
    CanvasView view{kCanvasViewInvalid};
    Window window{kWindowInvalid};
    BenchmarkPhase phase{};
};

// Owned by the main thread. A canvas id indexes `canvases`, a canvas view id indexes `canvasViews`.
// Pointer and keyboard state is the devices', not any one view's.
struct AppState {
    std::array<WindowState, kWindowMax> windows;
    std::array<CanvasState, kCanvasMax> canvases;
    std::array<CanvasViewState, kCanvasViewMax> canvasViews;
    std::array<WindowLayout, kWindowMax> layouts; // indexed by window
    TileAtlasState atlas;
    TileTransfersState transfers;
    HistoryPoolState historyPool;
    NavigateState navigate;
    BrushAdjustState brushAdjust;
    std::array<BrushOutlineState, kWindowMax> brushOutlines; // indexed by window
    StrokeState stroke;
    TransformState transform;
    PasteState paste;
    CopyState copy;
    TagDragState tagDrag;
    EyedropperState eyedropper;
    ExportState exporting;
    BenchmarkState benchmark;
    ToolsState tools;
    std::chrono::steady_clock::time_point brushCheckTime; // debug builds: when the shaders' files were last looked at
    // The last wheel notch or zoom key, and its view: the steps that follow it quickly join its navigation history
    // step.
    std::chrono::steady_clock::time_point viewStepTime;
    CanvasView viewStepView{kCanvasViewInvalid};
    std::chrono::steady_clock::time_point autosaveTime; // the last autosave, or the launch
    bool autosaveInput{};                               // a key or button since then: something may have changed
    bool exportBackground{true};                        // the export dialog's check box, as last left this run
    std::vector<std::filesystem::path> brushExamples;   // the .mdsl files in brushes/ next to the exe, by name
    KeymapState keymap;                                 // keybinds.json beside preferences.json, else kDefaultBindings
    std::vector<ActionId> recentActions;                // the most recent first, up to kRecentActionsMax
    std::vector<ActionRequest> actionRequests;          // picked by the UI, run by the next AppUpdate
    std::vector<std::filesystem::path> recentFiles; // opened or saved as, the most recent first, up to kRecentFilesMax
    std::vector<OpenFile> openFiles;                // of the windows closed since the exit began, in closing order
    std::string language;                           // the preference, a code of kLanguages or empty for the system's
    WindowCreateInfo windowPlacement; // the last window closed, or the first open at exit; its title unused
    CanvasFilter canvasFilter{CanvasFilter::Lanczos2}; // every view's
    float zoomMin{kViewportZoomMin};                   // the settings' limits of every view
    float zoomMax{kViewportZoomMax};
    // Shared by every window. Bumped at the start of a frame, right before the views report their visible
    // tile grid positions, so a tile with an older `visibleFrame` is shown nowhere.
    double pointerTime; // the last pointer event's
    std::uint32_t frame;
    float pointerX;     // last pointer position, client-area physical pixels of the window it moved in
    float pointerY;
    float pointerPressure;
    float pointerTiltX;
    float pointerTiltY;
    KeyMod keyMods;         // as of the last key event
    bool spaceDown;
    bool busy;              // AppBusy as of the last update: every window with a canvas presents each vsync
    bool modal;             // a dialog is up: its message loop still delivers events, which are dropped
    bool reopenFiles{true}; // the setting: a launch without files opens `openFiles`
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

// The view the keyboard, menus and panels act on: the active tab of the window's focused pane; kCanvasViewInvalid when
// that pane has none.
[[nodiscard]] CanvasView AppWindowView(Window window) noexcept;
// The panes' and views' rects from the window's client area.
void AppLayoutUpdate(Window window) noexcept;
// The views of the pane, by tab order, at most kCanvasViewMax.
[[nodiscard]] std::uint32_t AppPaneTabs(Window window, std::uint8_t pane,
                                        std::span<CanvasView, kCanvasViewMax> tabs) noexcept;
// The canvas's name as titles and tabs show it, with a * once modified.
[[nodiscard]] std::string AppCanvasTitle(Canvas canvas) noexcept;

// Each opens the canvas in a new tab of the window's focused pane; a file already open gets a view of its canvas, or
// that view when this window has one. Canceling any dialog is not an error. Failures are shown to the user, then
// returned.
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
void AppLayerToggleLocked(Window window) noexcept;
void AppLayerBlendMode(Window window, BlendMode blendMode) noexcept;
// The blend mode after the active layer's, or before it, wrapping around.
void AppLayerBlendModeCycle(Window window, bool next) noexcept;

// On the window's canvas, outside history. A new tag is checked; nothing when all kTagMax exist.
void AppTagNew(Window window) noexcept;
void AppTagRename(Window window, std::uint32_t tag, const char* name) noexcept;
void AppTagHue(Window window, std::uint32_t tag, float hue) noexcept;
// Once the user confirms: the tag leaves every tile and its id is free for the next new tag. No undo.
void AppTagDelete(Window window, std::uint32_t tag) noexcept;

// A left drag picks a color rather than paints or selects: the current tool is a Color tool, or a brush in paint mode
// (a Layer tool's path, not erasing) while a tool.eyedropper binding is held.
[[nodiscard]] bool AppEyedropperActive() noexcept;

// Why `action` with `arg` would do nothing in `window` right now, Text::Count_ when it would run. The search and the
// menus show it greyed out with the reason.
[[nodiscard]] Text ActionBlocked(Window window, ActionId action, std::uint32_t arg) noexcept;

// Borderless over the window's monitor, or back to how it was.
[[nodiscard]] Result<void> AppToggleFullscreen(Window window) noexcept;

// The lowest zoom whose tiles the tile atlas holds at the window's size. Lower is allowed, but shows only part of them.
[[nodiscard]] float AppZoomSupported(Window window) noexcept;

// The window's selection and the active layer's pixels under it follow handles over the canvas until applied, as one
// undo step, or canceled.
[[nodiscard]] Result<void> AppTransformMode(Window window) noexcept;
void AppTransformEnd(bool apply) noexcept;
[[nodiscard]] bool AppTransforming() noexcept;
[[nodiscard]] std::optional<TransformBox> AppTransformBox(Window window) noexcept;

} // namespace Midori
