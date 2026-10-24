#include "midori/app.hpp"
#include "midori/graphics.hpp"
#include "midori/history.hpp"
#include "midori/job.hpp"
#include "midori/layer.hpp"
#include "midori/log.hpp"
#include "midori/stroke.hpp"
#include "midori/tile.hpp"
#include "midori/ui.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <limits>
#include <numbers>
#include <string>
#include <utility>
#include <string_view>
#include <system_error>

#include <tracy/Tracy.hpp>

#include "midori/version.h"

namespace Midori {

AppState gAppState{};

namespace {

// Navigation steps each view keeps.
constexpr std::size_t kNavigationHistoryMax = 256;

// Pointer travel, in DIPs, that doubles or halves the zoom.
constexpr float kZoomDragDips = 200.0f;
constexpr float kRotateSnap = std::numbers::pi_v<float> / 12.0f; // 15 degrees

constexpr std::string_view kCanvasTypeName = "Midori canvas";
constexpr std::string_view kCanvasExtension = "mori";

static_assert(kCanvasViewMax >= kWindowMax, "a window without a view always finds a free one");

// Every window has one view over its whole client area for now; with several, this becomes a hit test.
[[nodiscard]] CanvasView WindowCanvasView(Window window) noexcept {
    for (const CanvasViewState& view : gAppState.canvasViews) {
        if (view.id != kCanvasViewInvalid && view.window == window) {
            return view.id;
        }
    }
    return kCanvasViewInvalid;
}

[[nodiscard]] bool CanvasShownElsewhere(Canvas canvas, CanvasView view) noexcept {
    return std::ranges::any_of(gAppState.canvasViews, [canvas, view](const CanvasViewState& other) {
        return other.id != kCanvasViewInvalid && other.id != view && other.canvas == canvas;
    });
}

// A canvas nobody can see is gone: its unsaved changes were saved or discarded before, see
// CanvasViewReleaseConfirmed.
void CanvasViewDetach(CanvasView view) noexcept {
    ZoneScopedN("CanvasViewDetach");
    CanvasViewState& viewState = gAppState.canvasViews[view];
    const Canvas canvas = viewState.canvas;
    viewState = {};
    if (gAppState.navigate.view == view) {
        gAppState.navigate = {};
    }
    if (gAppState.brushAdjust.view == view) {
        gAppState.brushAdjust = {};
    }
    if (gAppState.stroke.view == view) {
        StrokeRelease();
    }
    if (!CanvasShownElsewhere(canvas, kCanvasViewInvalid)) {
        CanvasDestroy(canvas);
    }
}

[[nodiscard]] std::string CanvasName(const CanvasState& canvas) noexcept {
    return canvas.path.empty() ? std::string("Untitled") : PathUtf8(canvas.path.filename());
}

void CanvasTitlesUpdate(Canvas canvas) noexcept {
    ZoneScopedN("CanvasTitlesUpdate");
    const CanvasState& state = gAppState.canvases[canvas];
    const std::string title = std::format("{}{} - {}", CanvasName(state), state.modified ? "*" : "", MIDORI_NAME);
    for (const CanvasViewState& view : gAppState.canvasViews) {
        if (view.id != kCanvasViewInvalid && view.canvas == canvas) {
            if (const auto res = WindowSetTitle(view.window, title); !res) {
                MIDORI_LOG_ERROR("WindowSetTitle failed: {}", ErrorMessage(res.error()));
            }
        }
    }
}

// The titles show the * once an edit modified the canvas.
void CanvasEdited(Canvas canvas, bool wasModified) noexcept {
    if (!wasModified && gAppState.canvases[canvas].modified) {
        CanvasTitlesUpdate(canvas);
    }
}

// Null when the window shows no canvas.
[[nodiscard]] CanvasState* WindowCanvas(Window window) noexcept {
    const CanvasView view = WindowCanvasView(window);
    return view == kCanvasViewInvalid ? nullptr : &gAppState.canvases[gAppState.canvasViews[view].canvas];
}

// Gives each of `layers` its `after`, as one undo step; stops at the first the canvas refuses.
void LayersPropertiesEdit(CanvasState& canvas, std::span<const Layer> layers,
                          std::span<const LayerProperties> after) noexcept {
    const bool wasModified = canvas.modified;
    std::vector<LayerProperties> before;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const LayerProperties previous = LayerPropertiesGet(canvas, layers[i]);
        if (!LayerPropertiesSet(canvas.id, layers[i], after[i])) {
            break;
        }
        before.push_back(previous);
    }
    LayerPropertiesCommit(canvas.id, layers.first(before.size()), before);
    CanvasEdited(canvas.id, wasModified);
}

// The window's view shows `canvas` from now on, with a fresh viewport.
void WindowShowCanvas(Window window, Canvas canvas) noexcept {
    ZoneScopedN("WindowShowCanvas");
    AppState& app = gAppState;
    CanvasView view = WindowCanvasView(window);
    if (view != kCanvasViewInvalid) {
        if (app.canvasViews[view].canvas == canvas) {
            return;
        }
        CanvasViewDetach(view);
    } else {
        const auto it = std::ranges::find(app.canvasViews, kCanvasViewInvalid, &CanvasViewState::id);
        MIDORI_ASSERT(it != app.canvasViews.end(), "no free canvas view");
        view = static_cast<CanvasView>(it - app.canvasViews.begin());
    }
    app.canvasViews[view] = CanvasViewState{.viewport = {}, .id = view, .canvas = canvas, .window = window};
    CanvasTitlesUpdate(canvas);
}

// Dialogs run their own message loop; AppEvent drops what arrives meanwhile.
[[nodiscard]] DialogChoice AppAskSave(Window window, std::string_view name) noexcept {
    ZoneScopedN("AppAskSave");
    const std::string instruction = std::format("Do you want to save changes to {}?", name);
    gAppState.modal = true;
    const DialogChoice choice = DialogAsk(DialogAskInfo{
        .title = MIDORI_NAME,
        .instruction = instruction,
        .content = "Your changes will be lost if you don't save them.",
        .yes = "Save",
        .no = "Don't Save",
        .window = window,
    });
    gAppState.modal = false;
    return choice;
}

void AppError(Window window, std::string_view instruction, Error error) noexcept {
    ZoneScopedN("AppError");
    const std::string content = std::format("Reason: {}.", ErrorMessage(error));
    gAppState.modal = true;
    DialogError(window, MIDORI_NAME, instruction, content);
    gAppState.modal = false;
}

[[nodiscard]] Result<std::filesystem::path> AppFileDialog(Window window, bool save,
                                                          const std::filesystem::path& initial) noexcept {
    ZoneScopedN("AppFileDialog");
    const DialogFileInfo info{
        .initial = initial,
        .typeName = kCanvasTypeName,
        .extension = kCanvasExtension,
        .window = window,
    };
    gAppState.modal = true;
    auto path = save ? DialogFileSave(info) : DialogFileOpen(info);
    gAppState.modal = false;
    if (!path) {
        AppError(window, "Could not show the file dialog", path.error());
    }
    return path;
}

// True once the canvas is on disk, false when the user canceled. Failures were shown to the user.
[[nodiscard]] Result<bool> CanvasSaveWithDialogs(Window window, Canvas canvas, bool saveAs) noexcept {
    ZoneScopedN("CanvasSaveWithDialogs");
    const CanvasState& state = gAppState.canvases[canvas];
    if (!saveAs && state.file.env) {
        if (const auto res = CanvasSave(canvas); !res) {
            AppError(window, std::format("Could not save {}", CanvasName(state)), res.error());
            return std::unexpected(res.error());
        }
        CanvasTitlesUpdate(canvas);
        return true;
    }

    std::filesystem::path initial = state.path;
    if (initial.empty()) {
        initial = "Untitled";
        initial.replace_extension(kCanvasExtension);
    }
    const auto path = AppFileDialog(window, true, initial);
    if (!path) {
        return std::unexpected(path.error());
    }
    if (path->empty()) {
        return false;
    }
    if (const auto res = CanvasSaveAs(canvas, *path); !res) {
        AppError(window, std::format("Could not save {}", PathUtf8(path->filename())), res.error());
        return std::unexpected(res.error());
    }
    CanvasTitlesUpdate(canvas);
    return true;
}

// Before `view` stops showing its canvas. False when the user cancels, or the save they asked for did not
// happen.
[[nodiscard]] bool CanvasViewReleaseConfirmed(CanvasView view) noexcept {
    ZoneScopedN("CanvasViewReleaseConfirmed");
    const Window window = gAppState.canvasViews[view].window;
    const Canvas canvas = gAppState.canvasViews[view].canvas;
    if (!gAppState.canvases[canvas].modified || CanvasShownElsewhere(canvas, view)) {
        return true;
    }
    switch (AppAskSave(window, CanvasName(gAppState.canvases[canvas]))) {
        case DialogChoice::Yes: {
            const auto saved = CanvasSaveWithDialogs(window, canvas, false);
            return saved && *saved;
        }
        case DialogChoice::No:
            return true;
        case DialogChoice::Cancel:
            return false;
    }
    return false;
}

[[nodiscard]] Result<void> WindowCanvasSave(Window window, bool saveAs) noexcept {
    ZoneScopedN("WindowCanvasSave");
    const CanvasView view = WindowCanvasView(window);
    if (view == kCanvasViewInvalid) {
        return {};
    }
    if (const auto saved = CanvasSaveWithDialogs(window, gAppState.canvasViews[view].canvas, saveAs); !saved) {
        return std::unexpected(saved.error());
    }
    return {};
}

// Replaces the window's canvas without asking: callers confirm first. Failures are shown to the user.
[[nodiscard]] Result<void> WindowOpenFile(Window window, const std::filesystem::path& path) noexcept {
    ZoneScopedN("WindowOpenFile");
    // Already open, and so locked: show that canvas.
    std::error_code ec;
    for (const CanvasState& canvas : gAppState.canvases) {
        if (canvas.id != kCanvasInvalid && !canvas.path.empty() && std::filesystem::equivalent(canvas.path, path, ec)) {
            WindowShowCanvas(window, canvas.id);
            return {};
        }
    }
    const auto canvas = CanvasOpen(path);
    if (!canvas) {
        AppError(window, std::format("Could not open {}", PathUtf8(path.filename())), canvas.error());
        return std::unexpected(canvas.error());
    }
    WindowShowCanvas(window, *canvas);
    return {};
}

// Ctrl+N new, Ctrl+O open, Ctrl+S save, Ctrl+Shift+S save as. Ctrl+Z undoes, Ctrl+Shift+Z and Ctrl+Y redo, repeating
// while held. B paints, E erases, F11 toggles fullscreen, Tab hides or shows the UI.
// Layers: Ctrl+Shift+N new layer, Ctrl+G new folder, Ctrl+Delete delete, Ctrl+E merge down or flatten a folder, Ctrl+,
// hide or show. Page Up and Page Down select above and below, with Ctrl move up and down, as in Krita: keys
// at the same place on every layout, unlike Photoshop's brackets. With Alt they step the blend mode.
[[nodiscard]] Result<void> KeyShortcut(const KeyEvent& key) noexcept {
    ZoneScopedN("KeyShortcut");
    const bool repeats = key.key == Key::Z || key.key == Key::Y;
    if (key.inputState != InputState::Press && !(repeats && key.inputState == InputState::Hold)) {
        return {};
    }
    if (key.keyMods == KeyMod::None && key.inputState == InputState::Press) {
        switch (key.key) {
            case Key::B:
                return AppPaintMode();
            case Key::E:
                return AppEraseMode();
            case Key::F11:
                return AppToggleFullscreen(key.window);
            case Key::PageUp:
            case Key::PageDown:
                AppLayerSelect(key.window, key.key == Key::PageUp);
                return {};
            case Key::Tab:
                if (key.window < kWindowMax) {
                    gUiState.hidden[key.window] = !gUiState.hidden[key.window];
                }
                return {};
            default:
                return {};
        }
    }
    // Not with Ctrl: AltGr comes as Ctrl+Alt.
    if (HasAnyFlag(key.keyMods, KeyMod::Alt) && !HasAnyFlag(key.keyMods, KeyMod::Ctrl | KeyMod::Shift)
        && key.inputState == InputState::Press && (key.key == Key::PageUp || key.key == Key::PageDown)) {
        AppLayerBlendModeCycle(key.window, key.key == Key::PageUp);
        return {};
    }
    if (!HasAnyFlag(key.keyMods, KeyMod::Ctrl)) {
        return {};
    }
    switch (key.key) {
        case Key::N:
            if (HasAnyFlag(key.keyMods, KeyMod::Shift)) {
                AppLayerNew(key.window, false);
                return {};
            }
            return AppCanvasNew(key.window);
        case Key::G:
            AppLayerNew(key.window, true);
            return {};
        case Key::E:
            AppLayerMerge(key.window);
            return {};
        case Key::Delete:
            AppLayerDelete(key.window);
            return {};
        case Key::PageUp:
        case Key::PageDown:
            AppLayerMove(key.window, key.key == Key::PageUp);
            return {};
        case Key::Comma:
            AppLayerToggleHidden(key.window);
            return {};
        case Key::O:
            return AppCanvasOpen(key.window);
        case Key::S:
            return HasAnyFlag(key.keyMods, KeyMod::Shift) ? AppCanvasSaveAs(key.window) : AppCanvasSave(key.window);
        case Key::Z:
            return HasAnyFlag(key.keyMods, KeyMod::Shift) ? AppRedo(key.window) : AppUndo(key.window);
        case Key::Y:
            return AppRedo(key.window);
        default:
            return {};
    }
}

struct ViewPoint {
    float x;
    float y;
};

// Client-area physical pixels to view DIPs around the view center, floored to a whole pixel like the
// renderer's projection.
[[nodiscard]] ViewPoint ViewPointFromPointer(const WindowState& window, float pointerX, float pointerY) noexcept {
    const float centerX = std::floor(static_cast<float>(window.width) * 0.5f);
    const float centerY = std::floor(static_cast<float>(window.height) * 0.5f);
    return ViewPoint{.x = (pointerX - centerX) / window.dpiScale, .y = (pointerY - centerY) / window.dpiScale};
}

[[nodiscard]] NavigateMode NavigateModeFromMods(KeyMod keyMods) noexcept {
    if (HasAnyFlag(keyMods, KeyMod::Ctrl)) {
        return NavigateMode::Zoom;
    }
    if (HasAnyFlag(keyMods, KeyMod::Shift)) {
        return NavigateMode::Rotate;
    }
    return NavigateMode::Pan;
}

// The canvas point under the last pointer position.
[[nodiscard]] Result<StrokePoint> CanvasPointUnderPointer(CanvasView view, float pressure) noexcept {
    ZoneScopedN("CanvasPointUnderPointer");
    const CanvasViewState& viewState = gAppState.canvasViews[view];
    const auto windowState = WindowGetState(viewState.window);
    if (!windowState) {
        return std::unexpected(windowState.error());
    }
    const ViewPoint point = ViewPointFromPointer(*windowState, gAppState.pointerX, gAppState.pointerY);
    // Moving the view by minus the point's offset brings the canvas point under it to the center.
    const Viewport under = ViewportMove(viewState.viewport, -point.x, -point.y);
    return StrokePoint{.x = under.posX, .y = under.posY, .pressure = pressure};
}

// Starts a frame: the root folder tiles any view's client area overlaps get the new frame as `visibleFrame`, and the
// grid positions there where a layer has a tile become the canvas's composite wants. Layer tiles are wanted by the
// composites that need them. A rotated view marks the tiles of its bounding box.
void TilesMarkVisible() noexcept {
    ZoneScopedN("TilesMarkVisible");
    const std::uint32_t frame = ++gAppState.frame;
    for (CanvasState& canvas : gAppState.canvases) {
        canvas.compositeWants.clear();
    }
    for (const CanvasViewState& view : gAppState.canvasViews) {
        if (view.id == kCanvasViewInvalid) {
            continue;
        }
        const auto windowState = WindowGetState(view.window);
        if (!windowState) {
            continue;
        }
        const auto width = static_cast<float>(windowState->width);
        const auto height = static_cast<float>(windowState->height);
        double minX = std::numeric_limits<double>::max();
        double minY = std::numeric_limits<double>::max();
        double maxX = std::numeric_limits<double>::lowest();
        double maxY = std::numeric_limits<double>::lowest();
        for (const auto [cornerX, cornerY] :
             {std::pair{0.0f, 0.0f}, std::pair{width, 0.0f}, std::pair{0.0f, height}, std::pair{width, height}}) {
            const ViewPoint point = ViewPointFromPointer(*windowState, cornerX, cornerY);
            const Viewport corner = ViewportMove(view.viewport, -point.x, -point.y);
            minX = std::min(minX, corner.posX);
            minY = std::min(minY, corner.posY);
            maxX = std::max(maxX, corner.posX);
            maxY = std::max(maxY, corner.posY);
        }
        const auto tileOf = [](double pos) {
            return static_cast<TileAxis>(std::floor(pos / kTileSize));
        };
        const TileGridPos min{.x = tileOf(minX), .y = tileOf(minY)};
        const TileGridPos max{.x = tileOf(maxX), .y = tileOf(maxY)};
        // The view's level, else level 0: where that level is missing, loading level 0 builds it; where it is
        // still loading, a loaded level 0 keeps showing meanwhile.
        CanvasState& canvas = gAppState.canvases[view.canvas];
        const TileMip level = TileLevelForZoom(view.viewport.zoomX);
        const Layer root = canvas.layersOrder.back();
        for (TileState& tile : canvas.tiles) {
            const TileGridPos gridPos = TileKeyGridPos(tile.key);
            if (gridPos.x < min.x || gridPos.x > max.x || gridPos.y < min.y || gridPos.y > max.y) {
                continue;
            }
            const TileMip mip = TileKeyMip(tile.key);
            const Layer layer = TileKeyLayer(tile.key);
            if (layer != root) {
                if (mip == 0 && !HasAnyFlag(tile.flags, TileFlags::Composite)
                    && canvas.layerHeights[layer] != kLayerInvalid) {
                    canvas.compositeWants.push_back(CompositeWant{.gridPos = TileGridPosKey(tile.key), .level = level});
                }
                continue;
            }
            // Composited again where its layers changed, or deleted where none is left.
            if (mip == 0) {
                canvas.compositeWants.push_back(CompositeWant{.gridPos = TileGridPosKey(tile.key), .level = level});
            }
            if (mip == level) {
                tile.visibleFrame = frame;
            } else if (mip == 0) {
                const auto found = canvas.tileIndices.find(TileKeyWithMip(tile.key, level));
                if (found == canvas.tileIndices.end()
                    || (!HasAnyFlag(canvas.tiles[found->second].flags, TileFlags::Loaded)
                        && HasAnyFlag(tile.flags, TileFlags::Loaded))) {
                    tile.visibleFrame = frame;
                }
            }
        }
    }
    // One want per grid position, at the finest level shown.
    for (CanvasState& canvas : gAppState.canvases) {
        std::vector<CompositeWant>& wants = canvas.compositeWants;
        std::ranges::sort(wants, [](const CompositeWant& a, const CompositeWant& b) {
            return a.gridPos < b.gridPos || (a.gridPos == b.gridPos && a.level < b.level);
        });
        const auto [first, last] = std::ranges::unique(wants, {}, &CompositeWant::gridPos);
        wants.erase(first, last);
    }
}

// Before the view leaves `previous`. The oldest steps go past kNavigationHistoryMax, and nothing is left to redo.
void NavigationPush(CanvasView view, const Viewport& previous) noexcept {
    CanvasViewState& state = gAppState.canvasViews[view];
    if (state.viewportUndo.size() == kNavigationHistoryMax) {
        state.viewportUndo.erase(state.viewportUndo.begin());
    }
    state.viewportUndo.push_back(previous);
    state.viewportRedo.clear();
}

// Back to the previous viewport, or forward again with `redo`.
void NavigationStep(CanvasView view, bool redo) noexcept {
    ZoneScopedN("NavigationStep");
    CanvasViewState& state = gAppState.canvasViews[view];
    std::vector<Viewport>& from = redo ? state.viewportRedo : state.viewportUndo;
    std::vector<Viewport>& to = redo ? state.viewportUndo : state.viewportRedo;
    if (from.empty()) {
        return;
    }
    to.push_back(state.viewport);
    state.viewport = from.back();
    from.pop_back();
}

[[nodiscard]] bool ViewportEqual(const Viewport& a, const Viewport& b) noexcept {
    return a.posX == b.posX && a.posY == b.posY && a.zoomX == b.zoomX && a.zoomY == b.zoomY && a.rot == b.rot;
}

// Starts a drag, or switches the one in progress to `mode` from where it is.
void NavigateBegin(CanvasView view, NavigateMode mode) noexcept {
    ZoneScopedN("NavigateBegin");
    const Viewport& viewport = gAppState.canvasViews[view].viewport;
    const bool dragging = gAppState.navigate.mode != NavigateMode::None;
    gAppState.navigate = NavigateState{
        .viewportStart = viewport,
        .gestureStart = dragging ? gAppState.navigate.gestureStart : viewport,
        .pointerStartX = gAppState.pointerX,
        .pointerStartY = gAppState.pointerY,
        .view = view,
        .mode = mode,
    };
}

// The zoom nearest `zoom` at which texels land on whole pixels or in small ratios at some level: whole
// magnifications above 1, else 2^-k or 2/3 * 2^-k (1, 2/3, 1/2, 1/3, 1/4, 1/6...).
[[nodiscard]] float ZoomSnap(float zoom) noexcept {
    if (zoom >= 1.0f) {
        return std::round(zoom);
    }
    const float upper = std::exp2(-std::floor(-std::log2(zoom)));
    float best = upper;
    for (const float candidate : {upper * (2.0f / 3.0f), upper * 0.5f}) {
        if (std::abs(std::log(zoom / candidate)) < std::abs(std::log(zoom / best))) {
            best = candidate;
        }
    }
    return best;
}

void NavigateUpdate() noexcept {
    ZoneScopedN("NavigateUpdate");
    const NavigateState& navigate = gAppState.navigate;
    CanvasViewState& view = gAppState.canvasViews[navigate.view];
    const auto windowState = WindowGetState(view.window);
    if (!windowState) {
        return;
    }
    const auto [startX, startY] = ViewPointFromPointer(*windowState, navigate.pointerStartX, navigate.pointerStartY);
    const auto [x, y] = ViewPointFromPointer(*windowState, gAppState.pointerX, gAppState.pointerY);
    const Viewport start = navigate.viewportStart;

    switch (navigate.mode) {
        case NavigateMode::Pan:
            view.viewport = ViewportMove(start, x - startX, y - startY);
            break;
        case NavigateMode::Zoom: {
            // Right or up zooms in.
            float amount = std::exp2(((x - startX) - (y - startY)) / kZoomDragDips);
            if (HasAnyFlag(gAppState.keyMods, KeyMod::Alt)) {
                const float zoom = std::abs(start.zoomX);
                amount = ZoomSnap(zoom * amount) / zoom;
            }
            view.viewport = ViewportZoom(start, amount, amount);
            break;
        }
        case NavigateMode::Rotate: {
            // A single flip mirrors the turn on screen, so it is reversed to keep following the pointer.
            const bool mirrored = (start.zoomX < 0.0f) != (start.zoomY < 0.0f);
            const float turn = std::atan2(y, x) - std::atan2(startY, startX);
            view.viewport = ViewportRotate(start, mirrored ? -turn : turn);
            if (HasAnyFlag(gAppState.keyMods, KeyMod::Alt)) {
                view.viewport.rot = std::remainder(std::round(view.viewport.rot / kRotateSnap) * kRotateSnap,
                                                   2.0f * std::numbers::pi_v<float>);
            }
            break;
        }
        case NavigateMode::None:
            break;
    }
    view.viewport = ViewportSnap(view.viewport, windowState->dpiScale);
}

// A drag that left the view somewhere else is one history step.
void NavigateEnd() noexcept {
    const NavigateState& navigate = gAppState.navigate;
    if (navigate.mode != NavigateMode::None
        && !ViewportEqual(navigate.gestureStart, gAppState.canvasViews[navigate.view].viewport)) {
        NavigationPush(navigate.view, navigate.gestureStart);
    }
    gAppState.navigate = {};
}

// Space + left drag pans, Ctrl+Space + drag zooms and Shift+Space + drag rotates, around the view center. Holding Alt
// snaps the zoom (ZoomSnap) and the rotation to 15 degrees. Space+R resets zoom and rotation, Space+F flips the view
// horizontally and Space+Shift+F vertically. Space+Z steps back
// through the navigation history and Space+Shift+Z forward, repeating while held, when no drag is in progress.
// Pressing or releasing Ctrl or Shift mid-drag switches mode from where the pointer is. Returns true when
// the event belongs to navigation. Releases and focus loss are handled even when the UI wants them, so a
// drag never sticks.
[[nodiscard]] bool Navigate(const Event& event, bool uiWants) noexcept {
    ZoneScopedN("Navigate");
    AppState& app = gAppState;
    switch (event.type) {
        case EventType::Key: {
            const bool space = event.key.key == Key::Space;
            const bool wasDown = app.spaceDown;
            if (space && event.key.inputState == InputState::Press && !uiWants) {
                app.spaceDown = true;
            } else if (space && event.key.inputState == InputState::Release) {
                app.spaceDown = false;
                NavigateEnd();
            }
            if (const CanvasView view = WindowCanvasView(event.key.window);
                event.key.key == Key::R && event.key.inputState == InputState::Press && app.spaceDown
                && view != kCanvasViewInvalid && !uiWants) {
                const auto windowState = WindowGetState(event.key.window);
                const float dpiScale = windowState ? windowState->dpiScale : 1.0f;
                // During a drag the reset is part of its step.
                if (app.navigate.mode == NavigateMode::None) {
                    NavigationPush(view, app.canvasViews[view].viewport);
                }
                app.canvasViews[view].viewport =
                    ViewportSnap(ViewportResetZoomRotation(app.canvasViews[view].viewport), dpiScale);
                // A drag in progress continues from the reset view instead of undoing it.
                if (app.navigate.mode != NavigateMode::None) {
                    NavigateBegin(app.navigate.view, app.navigate.mode);
                }
                return true;
            }
            if (const CanvasView view = WindowCanvasView(event.key.window);
                event.key.key == Key::F && event.key.inputState == InputState::Press && app.spaceDown
                && view != kCanvasViewInvalid && !uiWants) {
                // Like the reset: one step on its own, or part of the drag in progress, which goes on from the flip.
                if (app.navigate.mode == NavigateMode::None) {
                    NavigationPush(view, app.canvasViews[view].viewport);
                }
                const bool vertical = HasAnyFlag(app.keyMods, KeyMod::Shift);
                app.canvasViews[view].viewport =
                    ViewportZoom(app.canvasViews[view].viewport, vertical ? 1.0f : -1.0f, vertical ? -1.0f : 1.0f);
                if (app.navigate.mode != NavigateMode::None) {
                    NavigateBegin(app.navigate.view, app.navigate.mode);
                }
                return true;
            }
            if (const CanvasView view = WindowCanvasView(event.key.window);
                event.key.key == Key::Z && event.key.inputState != InputState::Release && app.spaceDown
                && view != kCanvasViewInvalid && !uiWants) {
                if (app.navigate.mode == NavigateMode::None) {
                    NavigationStep(view, HasAnyFlag(app.keyMods, KeyMod::Shift));
                }
                return true;
            }
            const NavigateMode mode = NavigateModeFromMods(app.keyMods);
            if (app.navigate.mode != NavigateMode::None && app.navigate.mode != mode) {
                NavigateBegin(app.navigate.view, mode);
            } else if (app.navigate.mode != NavigateMode::None) {
                // Alt pressed or released snaps or frees the view right away.
                NavigateUpdate();
            }
            return space && (wasDown || app.spaceDown);
        }
        case EventType::Button: {
            if (event.button.button != Button::Left) {
                return false;
            }
            if (event.button.inputState == InputState::Release && app.navigate.mode != NavigateMode::None) {
                NavigateEnd();
                return true;
            }
            const CanvasView view = WindowCanvasView(event.button.window);
            if (view != kCanvasViewInvalid && event.button.inputState == InputState::Press && app.spaceDown
                && !uiWants) {
                NavigateBegin(view, NavigateModeFromMods(app.keyMods));
                return true;
            }
            return false;
        }
        case EventType::Pointer: {
            if (app.navigate.mode == NavigateMode::None
                || app.canvasViews[app.navigate.view].window != event.pointer.window) {
                return false;
            }
            NavigateUpdate();
            return true;
        }
        case EventType::WindowFocus:
            if (!event.windowFocus.focused) {
                app.spaceDown = false;
                NavigateEnd();
            }
            return false;
        default:
            return false;
    }
}

[[nodiscard]] float& BrushAdjustTarget(BrushState& brush, BrushAdjustMode mode) noexcept {
    switch (mode) {
        case BrushAdjustMode::Radius:
            return brush.radius;
        case BrushAdjustMode::Opacity:
            return brush.opacity;
        case BrushAdjustMode::Hardness:
        case BrushAdjustMode::LayerOpacity:
        case BrushAdjustMode::None:
            break;
    }
    return brush.hardness;
}

[[nodiscard]] float BrushAdjustValue(const BrushAdjustState& adjust) noexcept {
    if (adjust.mode == BrushAdjustMode::LayerOpacity) {
        return gAppState.canvases[gAppState.canvasViews[adjust.view].canvas].layers[adjust.layer].opacity;
    }
    return BrushAdjustTarget(ToolBrush(gAppState.tools, adjust.tool), adjust.mode);
}

void BrushAdjustValueSet(const BrushAdjustState& adjust, float value) noexcept {
    if (adjust.mode != BrushAdjustMode::LayerOpacity) {
        BrushAdjustTarget(ToolBrush(gAppState.tools, adjust.tool), adjust.mode) = value;
        return;
    }
    const Canvas canvas = gAppState.canvasViews[adjust.view].canvas;
    const bool wasModified = gAppState.canvases[canvas].modified;
    LayerProperties properties = LayerPropertiesGet(gAppState.canvases[canvas], adjust.layer);
    properties.opacity = value;
    if (LayerPropertiesSet(canvas, adjust.layer, properties)) {
        CanvasEdited(canvas, wasModified);
    }
}

// A layer opacity kept is one undo step from where it started.
void BrushAdjustKeep() noexcept {
    BrushAdjustState& adjust = gAppState.brushAdjust;
    if (adjust.mode == BrushAdjustMode::LayerOpacity) {
        const Canvas canvas = gAppState.canvasViews[adjust.view].canvas;
        LayerProperties before = LayerPropertiesGet(gAppState.canvases[canvas], adjust.layer);
        before.opacity = adjust.startValue;
        LayerPropertiesCommit(canvas, std::span(&adjust.layer, 1), std::span(&before, 1));
    }
    adjust = {};
}

[[nodiscard]] float BrushAdjustPixelsPerUnit(const BrushAdjustState& adjust, float dpiScale) noexcept {
    if (adjust.mode == BrushAdjustMode::Radius) {
        return std::abs(gAppState.canvasViews[adjust.view].viewport.zoomX) * dpiScale;
    }
    return kBrushAdjustRangeDips * dpiScale;
}

// Alt snaps to a power of two, as it snaps navigation; a layer opacity to a multiple of 5%.
void BrushAdjustApply() noexcept {
    ZoneScopedN("BrushAdjustApply");
    const BrushAdjustState& adjust = gAppState.brushAdjust;
    float value = adjust.value;
    if (HasAnyFlag(gAppState.keyMods, KeyMod::Alt) && adjust.mode == BrushAdjustMode::LayerOpacity) {
        value = std::round(value * 20.0f) / 20.0f;
    } else if (HasAnyFlag(gAppState.keyMods, KeyMod::Alt) && value > 0.0f) {
        value = std::exp2(std::round(std::log2(value)));
    }
    value = adjust.mode == BrushAdjustMode::Radius ? std::clamp(value, kBrushRadiusMin, kBrushRadiusMax)
                                                   : std::clamp(value, 0.0f, 1.0f);
    // Whole canvas units from 1 up; only a sub-unit brush keeps a fraction. `adjust.value` keeps the fraction, so Shift
    // still moves it smoothly toward the next whole unit.
    if (adjust.mode == BrushAdjustMode::Radius && value >= 1.0f) {
        value = std::round(value);
    }
    BrushAdjustValueSet(adjust, value);
}

void BrushAdjustCancel() noexcept {
    ZoneScopedN("BrushAdjustCancel");
    BrushAdjustState& adjust = gAppState.brushAdjust;
    BrushAdjustValueSet(adjust, adjust.startValue);
    adjust = {};
}

// F sets the current tool's brush radius, Shift+F its opacity and Ctrl+F its hardness, and O the active layer's
// opacity, from the pointer's distance to a center placed so the current value starts under the pointer. Holding
// Shift moves it 10 times slower and Alt snaps it to powers of two, a layer opacity to multiples of 5%, together or
// not. Left click or Enter keeps the value, right click or Escape restores it. While adjusting, every input event
// belongs to it.
[[nodiscard]] bool BrushAdjust(const Event& event, bool uiWants) noexcept {
    ZoneScopedN("BrushAdjust");
    AppState& app = gAppState;
    BrushAdjustState& adjust = app.brushAdjust;
    if (adjust.mode == BrushAdjustMode::None) {
        if (event.type != EventType::Key || (event.key.key != Key::F && event.key.key != Key::O)
            || event.key.inputState != InputState::Press || uiWants || app.spaceDown
            || app.navigate.mode != NavigateMode::None || (app.stroke.canvas != kCanvasInvalid && !app.stroke.ended)) {
            return false;
        }
        BrushAdjustMode mode = BrushAdjustMode::None;
        if (event.key.key == Key::O) {
            mode = event.key.keyMods == KeyMod::None ? BrushAdjustMode::LayerOpacity : BrushAdjustMode::None;
        } else if (event.key.keyMods == KeyMod::None) {
            mode = BrushAdjustMode::Radius;
        } else if (HasAnyFlag(event.key.keyMods, KeyMod::Shift) && !HasAnyFlag(event.key.keyMods, ~KeyMod::Shift)) {
            mode = BrushAdjustMode::Opacity;
        } else if (HasAnyFlag(event.key.keyMods, KeyMod::Ctrl) && !HasAnyFlag(event.key.keyMods, ~KeyMod::Ctrl)) {
            mode = BrushAdjustMode::Hardness;
        }
        const CanvasView view = WindowCanvasView(event.key.window);
        const auto windowState = WindowGetState(event.key.window);
        if (mode == BrushAdjustMode::None || view == kCanvasViewInvalid || !windowState) {
            return false;
        }
        const CanvasState& canvas = app.canvases[app.canvasViews[view].canvas];
        if (mode == BrushAdjustMode::LayerOpacity
            && (canvas.activeLayer == kLayerInvalid || canvas.merge.result != kLayerInvalid)) {
            return false;
        }
        adjust = BrushAdjustState{.layer = canvas.activeLayer, .view = view, .tool = app.tools.tool, .mode = mode};
        adjust.startValue = BrushAdjustValue(adjust);
        adjust.value = adjust.startValue;
        adjust.distance = adjust.value * BrushAdjustPixelsPerUnit(adjust, windowState->dpiScale);
        adjust.centerX = app.pointerX - adjust.distance;
        adjust.centerY = app.pointerY;
        return true;
    }

    switch (event.type) {
        case EventType::Key:
            if (event.key.inputState == InputState::Press
                && (event.key.key == Key::Enter || event.key.key == Key::NumPadEnter)) {
                BrushAdjustKeep();
            } else if (event.key.inputState == InputState::Press && event.key.key == Key::Escape) {
                BrushAdjustCancel();
            } else {
                BrushAdjustApply();
            }
            return true;
        case EventType::Button:
            if (event.button.inputState == InputState::Press && event.button.button == Button::Left) {
                BrushAdjustKeep();
            } else if (event.button.inputState == InputState::Press && event.button.button == Button::Right) {
                BrushAdjustCancel();
            }
            return true;
        case EventType::Pointer: {
            const Window window = app.canvasViews[adjust.view].window;
            const auto windowState = WindowGetState(window);
            if (event.pointer.window != window || !windowState) {
                return true;
            }
            const float distance = std::hypot(event.pointer.posX - adjust.centerX, event.pointer.posY - adjust.centerY);
            const float speed = HasAnyFlag(app.keyMods, KeyMod::Shift) ? 0.1f : 1.0f;
            adjust.value +=
                (distance - adjust.distance) * speed / BrushAdjustPixelsPerUnit(adjust, windowState->dpiScale);
            adjust.distance = distance;
            BrushAdjustApply();
            return true;
        }
        case EventType::Text:
        case EventType::Wheel:
            return true;
        case EventType::WindowFocus:
            if (!event.windowFocus.focused) {
                BrushAdjustKeep();
            }
            return false;
        default:
            return false;
    }
}

// The UI's cursor over the UI. Over the canvas: navigation's while Space is held, else none, the brush outline showing
// where the paint or erase lands. Zoom drags right or up, like the NE-SW arrow; rotation has no system cursor.
[[nodiscard]] WindowCursor AppCursor(Window window) noexcept {
    ZoneScopedN("AppCursor");
    const AppState& app = gAppState;
    if (const auto ui = UiCursor(window)) {
        return *ui;
    }
    const CanvasView view = WindowCanvasView(window);
    if (view == kCanvasViewInvalid) {
        return WindowCursor::Arrow;
    }
    NavigateMode mode = NavigateMode::None;
    if (app.navigate.mode != NavigateMode::None && app.navigate.view == view) {
        mode = app.navigate.mode;
    } else if (app.spaceDown) {
        mode = NavigateModeFromMods(app.keyMods);
    }
    switch (mode) {
        case NavigateMode::Pan:
            return WindowCursor::Move;
        case NavigateMode::Zoom:
            return WindowCursor::ResizeNESW;
        case NavigateMode::Rotate:
            return WindowCursor::None;
        case NavigateMode::None:
            break;
    }
    return WindowCursor::None;
}

// The current tool's brush under the pointer, over the canvas only: not over the UI, while navigating, or while
// adjusting the brush, which shows its own. While painting or erasing, the radius the stroke paints at now.
[[nodiscard]] BrushOutlineState BrushOutline(Window window) noexcept {
    ZoneScopedN("BrushOutline");
    const AppState& app = gAppState;
    const CanvasView view = WindowCanvasView(window);
    const auto windowState = WindowGetState(window);
    if (view == kCanvasViewInvalid || !windowState || !windowState->hovered || UiCursor(window) || app.spaceDown
        || app.navigate.mode != NavigateMode::None || app.brushAdjust.mode != BrushAdjustMode::None) {
        return {};
    }
    const float scale = std::abs(app.canvasViews[view].viewport.zoomX) * windowState->dpiScale;
    const StrokeState& stroke = app.stroke;
    const bool painting = stroke.canvas != kCanvasInvalid && !stroke.ended && stroke.view == view;
    return BrushOutlineState{
        .x = app.pointerX,
        .y = app.pointerY,
        .radius =
            (painting ? DabRadius(stroke.brush, stroke.last.pressure) : ToolBrush(app.tools, app.tools.tool).radius)
            * scale,
        .lineWidth = std::max(1.0f, std::round(windowState->dpiScale)),
        .shown = true,
    };
}

[[nodiscard]] StrokeBeginInfo StrokeBeginInfoFromTool(CanvasView view, StrokePoint point) noexcept {
    const ToolsState& tools = gAppState.tools;
    const Layer layer = gAppState.canvases[gAppState.canvasViews[view].canvas].activeLayer;
    if (tools.tool == Tool::Erase) {
        return StrokeBeginInfo{
            .brush = tools.erase.brush,
            .color = tools.erase.color,
            .point = point,
            .layer = layer,
            .view = view,
            .mode = StrokeMode::Erase,
            .blendMode = BlendMode::Normal,
        };
    }
    return StrokeBeginInfo{
        .brush = tools.paint.brush,
        .color = tools.paint.color,
        .point = point,
        .layer = layer,
        .view = view,
        .mode = StrokeMode::Paint,
        .blendMode = tools.paint.blendMode,
    };
}

// Left drag paints or erases the active layer, by the current tool. Releases and focus loss end the stroke even when
// the UI wants them. Returns true when the event belongs to the stroke.
[[nodiscard]] bool Paint(const Event& event, bool uiWants) noexcept {
    ZoneScopedN("Paint");
    StrokeState& stroke = gAppState.stroke;
    const bool painting = stroke.canvas != kCanvasInvalid && !stroke.ended;
    switch (event.type) {
        case EventType::Button: {
            if (event.button.button != Button::Left) {
                return false;
            }
            if (event.button.inputState == InputState::Release) {
                if (painting) {
                    StrokeEnd();
                }
                return painting;
            }
            const CanvasView view = WindowCanvasView(event.button.window);
            // An ended stroke the GPU has not finished yet blocks the next one for a frame at most, and so do undo
            // and redo steps waiting for their tiles. A merge blocks it until done.
            if (event.button.inputState != InputState::Press || uiWants || view == kCanvasViewInvalid
                || stroke.canvas != kCanvasInvalid) {
                return false;
            }
            if (const CanvasState& target = gAppState.canvases[gAppState.canvasViews[view].canvas];
                target.history.pending != 0 || target.merge.result != kLayerInvalid
                || !LayerPaintable(target, target.activeLayer)) {
                return false;
            }
            // The mouse has no pressure: 1 while a button is held.
            const float pressure = event.button.device == kDeviceSystem ? 1.0f : gAppState.pointerPressure;
            const auto point = CanvasPointUnderPointer(view, pressure);
            if (!point) {
                return false;
            }
            const Canvas canvas = gAppState.canvasViews[view].canvas;
            const bool wasModified = gAppState.canvases[canvas].modified;
            StrokeBegin(StrokeBeginInfoFromTool(view, *point));
            CanvasEdited(canvas, wasModified);
            return true;
        }
        case EventType::Pointer: {
            if (!painting || gAppState.canvasViews[stroke.view].window != event.pointer.window) {
                return false;
            }
            if (const auto point = CanvasPointUnderPointer(stroke.view, event.pointer.pressure)) {
                const bool wasModified = gAppState.canvases[stroke.canvas].modified;
                StrokeTo(*point);
                CanvasEdited(stroke.canvas, wasModified);
            }
            return true;
        }
        case EventType::WindowFocus:
            if (painting && !event.windowFocus.focused) {
                StrokeEnd();
            }
            return false;
        default:
            return false;
    }
}

} // namespace

Result<void> AppInit(AppInitInfo initInfo) noexcept {
    ZoneScopedN("AppInit");
    MIDORI_LOG_INFO("{} v{} {}-{} {}", MIDORI_NAME, MIDORI_VERSION_STRING, MIDORI_COMPILER, MIDORI_ARCHITECTURE,
                    MIDORI_PROFILE);

    // GraphicsInit created atlas 0. The clear page is held by no tile.
    gAppState.atlas.count = 1;
    gAppState.atlas.used[kTileAtlasPageClear] = ~std::uint64_t{0};
    // Largest first, so pop_back gives the smallest.
    gAppState.historyPool.freeSlots.reserve(kHistorySlotsMax);
    for (std::uint32_t slot = kHistorySlotsMax; slot > 0; --slot) {
        gAppState.historyPool.freeSlots.push_back(slot - 1);
    }
    JobsInit(JobsInitInfo{.execute = CanvasJobExecute, .readerCount = 0});

    // One window per file, or one new canvas. A file that fails to open leaves a new canvas in its window.
    // The platform owns the windows and destroys them in PlatformTerminate.
    const std::size_t windowCount = std::max<std::size_t>(initInfo.files.size(), 1);
    for (std::size_t i = 0; i < windowCount; ++i) {
        const auto window = WindowCreate(WindowCreateInfo{.title = MIDORI_NAME});
        if (!window) {
            MIDORI_LOG_ERROR("WindowCreate failed: {}", ErrorMessage(window.error()));
            // Files already shown keep the app running; without any window it cannot start.
            return i == 0 ? std::unexpected(window.error()) : Result<void>{};
        }
        if (i < initInfo.files.size() && WindowOpenFile(*window, initInfo.files[i])) {
            continue;
        }
        if (const auto res = AppCanvasNew(*window); !res) {
            MIDORI_LOG_ERROR("AppCanvasNew failed: {}", ErrorMessage(res.error()));
            return std::unexpected(res.error());
        }
    }
    return {};
}

Result<void> AppEvent(Event event) noexcept {
    ZoneScopedN("AppEvent");
    // Only what keeps the windows consistent gets through while a dialog is up.
    if (gAppState.modal && event.type != EventType::WindowResize && event.type != EventType::WindowFocus
        && event.type != EventType::WindowDestroy) {
        return {};
    }
    if (event.type == EventType::Key) {
        gAppState.keyMods = event.key.keyMods;
    } else if (event.type == EventType::Pointer) {
        gAppState.pointerX = event.pointer.posX;
        gAppState.pointerY = event.pointer.posY;
        gAppState.pointerPressure = event.pointer.pressure;
    }
    const bool uiWants = UiEvent(event);
    if (BrushAdjust(event, uiWants) || Navigate(event, uiWants) || Paint(event, uiWants) || uiWants) {
        return {};
    }
    switch (event.type) {
        case EventType::WindowResize:
            MIDORI_LOG_TRACE("window {}: resize {}x{} scale {}", event.windowResize.window, event.windowResize.newWidth,
                             event.windowResize.newHeight, event.windowResize.dpiScale);
            break;
        case EventType::WindowFocus:
            MIDORI_LOG_TRACE("window {}: focus {}", event.windowFocus.window, event.windowFocus.focused);
            break;
        case EventType::WindowClose:
            return AppWindowClose(event.windowClose.window);
        case EventType::WindowDestroy:
            // The window owns its views.
            for (CanvasViewState& view : gAppState.canvasViews) {
                if (view.id != kCanvasViewInvalid && view.window == event.windowDestroy.window) {
                    CanvasViewDetach(view.id);
                }
            }
            break;
        case EventType::Key:
            MIDORI_LOG_TRACE("window {}: key {} state {} mods {:#04x}", event.key.window,
                             std::to_underlying(event.key.key), std::to_underlying(event.key.inputState),
                             std::to_underlying(event.key.keyMods));
            return KeyShortcut(event.key);
        case EventType::Text:
            MIDORI_LOG_TRACE("window {}: text U+{:04X}", event.text.window,
                             static_cast<std::uint32_t>(event.text.codepoint));
            break;
        case EventType::Button:
            MIDORI_LOG_TRACE("window {}: device {} button {} state {}", event.button.window, event.button.device,
                             std::to_underlying(event.button.button), std::to_underlying(event.button.inputState));
            break;
        case EventType::Pointer:
            MIDORI_LOG_TRACE("window {}: device {} pointer {:.2f},{:.2f} pressure {:.3f} tilt {:.2f},{:.2f}",
                             event.pointer.window, event.pointer.device, event.pointer.posX, event.pointer.posY,
                             event.pointer.pressure, event.pointer.tiltX, event.pointer.tiltY);
            break;
        case EventType::Wheel:
            MIDORI_LOG_TRACE("window {}: device {} wheel {:.2f},{:.2f}", event.wheel.window, event.wheel.device,
                             event.wheel.deltaX, event.wheel.deltaY);
            break;
        case EventType::Unknown:
            break;
    }
    return {};
}

void AppWindowFrame(Window window) noexcept {
    ZoneScopedN("AppWindowFrame");
    const CanvasView view = WindowCanvasView(window);
    Viewport* viewport = nullptr;
    Color::Srgb* backgroundColor = nullptr;
    if (view != kCanvasViewInvalid) {
        CanvasViewState& viewState = gAppState.canvasViews[view];
        viewport = &viewState.viewport;
        backgroundColor = &gAppState.canvases[viewState.canvas].backgroundColor;
    }
    const Viewport before = viewport ? *viewport : Viewport{};
    const bool wasModified =
        view != kCanvasViewInvalid && gAppState.canvases[gAppState.canvasViews[view].canvas].modified;
    if (const UiCommand command = UiWindowFrame(window, viewport, backgroundColor, gAppState.tools);
        command != UiCommand::None) {
        gAppState.command = command;
        gAppState.commandWindow = window;
    }
    // The layers window edits the canvas.
    if (view != kCanvasViewInvalid) {
        CanvasEdited(gAppState.canvasViews[view].canvas, wasModified);
    }
    // The view panel's flips and reset.
    if (viewport && !ViewportEqual(before, *viewport)) {
        NavigationPush(view, before);
    }
    gAppState.brushOutlines[window] = BrushOutline(window);
    if (const auto res = WindowSetCursor(window, AppCursor(window)); !res) {
        MIDORI_LOG_ERROR("WindowSetCursor failed: {}", ErrorMessage(res.error()));
    }
    // ImGui animates and trickles queued input over several frames, so keep presenting while it is shown.
    GraphicsWindowRequestFrame(window);
}

Result<void> AppUpdate() noexcept {
    ZoneScopedN("AppUpdate");
    TilesMarkVisible();
    StrokeUpdate();
    for (const CanvasState& canvas : gAppState.canvases) {
        if (canvas.id != kCanvasInvalid) {
            const bool wasModified = canvas.modified;
            HistoryUpdate(canvas.id);
            CanvasEdited(canvas.id, wasModified);
        }
    }
    CanvasTilesStream();
    const UiCommand command = std::exchange(gAppState.command, UiCommand::None);
    const Window window = gAppState.commandWindow;
    if (command == UiCommand::None || !WindowGetState(window)) {
        return {};
    }
    switch (command) {
        case UiCommand::CanvasNew:
            return AppCanvasNew(window);
        case UiCommand::CanvasOpen:
            return AppCanvasOpen(window);
        case UiCommand::CanvasSave:
            return AppCanvasSave(window);
        case UiCommand::CanvasSaveAs:
            return AppCanvasSaveAs(window);
        case UiCommand::Undo:
            return AppUndo(window);
        case UiCommand::Redo:
            return AppRedo(window);
        case UiCommand::ToggleFullscreen:
            return AppToggleFullscreen(window);
        case UiCommand::Exit:
            // Closes every window, stopping at the first the user keeps open.
            for (Window other = 0; other < kWindowMax; ++other) {
                if (!WindowGetState(other)) {
                    continue;
                }
                if (const auto res = AppWindowClose(other); !res) {
                    return res;
                }
                if (WindowGetState(other)) {
                    break;
                }
            }
            return {};
        case UiCommand::None:
            break;
    }
    return {};
}

// The windows still open are destroyed after this, by PlatformTerminate.
void AppTerminate() noexcept {
    ZoneScopedN("AppTerminate");
    gAppState.canvasViews = {};
    for (const CanvasState& canvas : gAppState.canvases) {
        if (canvas.id != kCanvasInvalid) {
            CanvasDestroy(canvas.id);
        }
    }
    JobsTerminate();
}

Result<void> AppCanvasNew(Window window) noexcept {
    ZoneScopedN("AppCanvasNew");
    if (const CanvasView view = WindowCanvasView(window);
        view != kCanvasViewInvalid && !CanvasViewReleaseConfirmed(view)) {
        return {};
    }
    const auto canvas = CanvasNew();
    if (!canvas) {
        AppError(window, "Could not create a canvas", canvas.error());
        return std::unexpected(canvas.error());
    }
    WindowShowCanvas(window, *canvas);
    return {};
}

Result<void> AppCanvasOpen(Window window) noexcept {
    ZoneScopedN("AppCanvasOpen");
    if (const CanvasView view = WindowCanvasView(window);
        view != kCanvasViewInvalid && !CanvasViewReleaseConfirmed(view)) {
        return {};
    }
    const auto path = AppFileDialog(window, false, {});
    if (!path) {
        return std::unexpected(path.error());
    }
    if (path->empty()) {
        return {};
    }
    return WindowOpenFile(window, *path);
}

Result<void> AppCanvasSave(Window window) noexcept {
    ZoneScopedN("AppCanvasSave");
    return WindowCanvasSave(window, false);
}

Result<void> AppCanvasSaveAs(Window window) noexcept {
    ZoneScopedN("AppCanvasSaveAs");
    return WindowCanvasSave(window, true);
}

Result<void> AppUndo(Window window) noexcept {
    ZoneScopedN("AppUndo");
    const CanvasView view = WindowCanvasView(window);
    if (view == kCanvasViewInvalid) {
        return {};
    }
    HistoryState& history = gAppState.canvases[gAppState.canvasViews[view].canvas].history;
    if (static_cast<std::int32_t>(history.current) + history.pending > 0) {
        if (history.pending == 0) {
            history.pendingFrame = gAppState.frame;
        }
        --history.pending;
    }
    return {};
}

Result<void> AppRedo(Window window) noexcept {
    ZoneScopedN("AppRedo");
    const CanvasView view = WindowCanvasView(window);
    if (view == kCanvasViewInvalid) {
        return {};
    }
    HistoryState& history = gAppState.canvases[gAppState.canvasViews[view].canvas].history;
    if (static_cast<std::int32_t>(history.current) + history.pending
        < static_cast<std::int32_t>(history.records.size())) {
        if (history.pending == 0) {
            history.pendingFrame = gAppState.frame;
        }
        ++history.pending;
    }
    return {};
}

Result<void> AppToggleFullscreen(Window window) noexcept {
    ZoneScopedN("AppToggleFullscreen");
    const auto state = WindowGetState(window);
    if (!state) {
        return std::unexpected(state.error());
    }
    const bool fullscreen = state->displayMode == WindowDisplayMode::Fullscreened;
    return WindowSetDisplayMode(window, fullscreen ? WindowDisplayMode::Normal : WindowDisplayMode::Fullscreened);
}

// A stroke in progress keeps the tool it began with.
Result<void> AppPaintMode() noexcept {
    ZoneScopedN("AppPaintMode");
    gAppState.tools.tool = Tool::Paint;
    return {};
}

Result<void> AppEraseMode() noexcept {
    ZoneScopedN("AppEraseMode");
    gAppState.tools.tool = Tool::Erase;
    return {};
}

void AppLayerNew(Window window, bool folder) noexcept {
    ZoneScopedN("AppLayerNew");
    CanvasState* canvas = WindowCanvas(window);
    if (!canvas) {
        return;
    }
    // Numbered after the layers, or folders, already there.
    int number = 1;
    for (const Layer id : canvas->layersOrder) {
        const LayerFlags flags = canvas->layers[id].flags;
        number += !HasAnyFlag(flags, LayerFlags::Internal) && HasAnyFlag(flags, LayerFlags::Folder) == folder ? 1 : 0;
    }
    std::array<char, kLayerNameLenMax> name{};
    std::format_to_n(name.data(), name.size() - 1, "{} {}", folder ? "Folder" : "Layer", number);
    const Layer root = static_cast<Layer>(canvas->layersOrder.size() - 1);
    const LayerCreateInfo createInfo{
        .height = canvas->activeLayer == kLayerInvalid
                    ? root
                    : static_cast<Layer>(canvas->layerHeights[canvas->activeLayer] + 1),
        .name = name,
        .flags = folder ? LayerFlags::Folder : LayerFlags::None,
    };
    const bool wasModified = canvas->modified;
    if (const auto layer = LayerNew(canvas->id, createInfo)) {
        canvas->activeLayer = *layer;
        canvas->selectedLayers.assign(1, *layer);
    }
    CanvasEdited(canvas->id, wasModified);
}

void AppLayerDelete(Window window) noexcept {
    ZoneScopedN("AppLayerDelete");
    CanvasState* canvas = WindowCanvas(window);
    if (!canvas) {
        return;
    }
    const bool wasModified = canvas->modified;
    if (const auto res = LayerDelete(canvas->id, LayersSelected(*canvas)); !res) {
        MIDORI_LOG_TRACE("AppLayerDelete: {}", ErrorMessage(res.error()));
    }
    CanvasEdited(canvas->id, wasModified);
}

void AppLayerMerge(Window window) noexcept {
    ZoneScopedN("AppLayerMerge");
    CanvasState* canvas = WindowCanvas(window);
    if (!canvas || canvas->activeLayer == kLayerInvalid) {
        return;
    }
    const bool wasModified = canvas->modified;
    if (const auto res = LayerMerge(canvas->id, canvas->activeLayer); !res) {
        MIDORI_LOG_TRACE("AppLayerMerge: {}", ErrorMessage(res.error()));
    }
    CanvasEdited(canvas->id, wasModified);
}

void AppLayerMove(Window window, bool up) noexcept {
    ZoneScopedN("AppLayerMove");
    CanvasState* canvas = WindowCanvas(window);
    if (!canvas || canvas->activeLayer == kLayerInvalid) {
        return;
    }
    const Layer layer = canvas->activeLayer;
    // In the order without the layer and what it holds, `bottom` is what was right above it.
    const auto bottom = static_cast<Layer>(canvas->layerHeights[layer] - canvas->layers[layer].descendants);
    if (!up && bottom == 0) {
        return;
    }
    const bool wasModified = canvas->modified;
    if (const auto res = LayerMove(canvas->id, layer, static_cast<Layer>(up ? bottom + 1 : bottom - 1)); !res) {
        MIDORI_LOG_TRACE("AppLayerMove: {}", ErrorMessage(res.error()));
    }
    CanvasEdited(canvas->id, wasModified);
}

void AppLayerSelect(Window window, bool up) noexcept {
    CanvasState* canvas = WindowCanvas(window);
    if (!canvas || canvas->layersOrder.size() < 2) {
        return;
    }
    const auto root = static_cast<Layer>(canvas->layersOrder.size() - 1);
    if (canvas->activeLayer == kLayerInvalid) {
        canvas->activeLayer = canvas->layersOrder[root - 1];
        canvas->selectedLayers.assign(1, canvas->activeLayer);
        return;
    }
    const Layer height = canvas->layerHeights[canvas->activeLayer];
    if (up && height + 1 < root) {
        canvas->activeLayer = canvas->layersOrder[height + 1];
    } else if (!up && height > 0) {
        canvas->activeLayer = canvas->layersOrder[height - 1];
    }
    canvas->selectedLayers.assign(1, canvas->activeLayer);
}

void AppLayerToggleHidden(Window window) noexcept {
    ZoneScopedN("AppLayerToggleHidden");
    CanvasState* canvas = WindowCanvas(window);
    if (!canvas) {
        return;
    }
    const std::vector<Layer> layers = LayersSelected(*canvas);
    if (layers.empty()) {
        return;
    }
    // All the same way, the opposite of the top one.
    const bool hidden = !LayerPropertiesGet(*canvas, layers.front()).hidden;
    std::vector<LayerProperties> after;
    for (const Layer layer : layers) {
        after.push_back(LayerPropertiesGet(*canvas, layer));
        after.back().hidden = hidden;
    }
    LayersPropertiesEdit(*canvas, layers, after);
}

void AppLayerBlendMode(Window window, BlendMode blendMode) noexcept {
    ZoneScopedN("AppLayerBlendMode");
    CanvasState* canvas = WindowCanvas(window);
    if (!canvas) {
        return;
    }
    const std::vector<Layer> layers = LayersSelected(*canvas);
    std::vector<LayerProperties> after;
    for (const Layer layer : layers) {
        after.push_back(LayerPropertiesGet(*canvas, layer));
        after.back().blendMode = blendMode;
    }
    LayersPropertiesEdit(*canvas, layers, after);
}

void AppLayerBlendModeCycle(Window window, bool next) noexcept {
    const CanvasState* canvas = WindowCanvas(window);
    if (!canvas || canvas->activeLayer == kLayerInvalid) {
        return;
    }
    constexpr int kCount = std::to_underlying(BlendMode::Count_);
    const int current = std::to_underlying(canvas->layers[canvas->activeLayer].blendMode);
    AppLayerBlendMode(window, static_cast<BlendMode>((current + (next ? 1 : kCount - 1)) % kCount));
}

Result<void> AppWindowClose(Window window) noexcept {
    ZoneScopedN("AppWindowClose");
    if (const CanvasView view = WindowCanvasView(window);
        view != kCanvasViewInvalid && !CanvasViewReleaseConfirmed(view)) {
        return {};
    }
    return WindowDestroy(window);
}

} // namespace Midori
