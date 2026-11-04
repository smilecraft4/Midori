#include "midori/app.hpp"
#include "midori/embed/round_mdsl.h"
#include "midori/graphics.hpp"
#include "midori/history.hpp"
#include "midori/job.hpp"
#include "midori/layer.hpp"
#include "midori/log.hpp"
#include "midori/preferences.hpp"
#include "midori/stroke.hpp"
#include "midori/tag.hpp"
#include "midori/text.hpp"
#include "midori/tile.hpp"
#include "midori/ui.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <numbers>
#include <optional>
#include <sstream>
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
    if (gAppState.eyedropper.view == view) {
        gAppState.eyedropper = {};
    }
    if (gAppState.stroke.view == view) {
        StrokeRelease();
    }
    if (!CanvasShownElsewhere(canvas, kCanvasViewInvalid)) {
        CanvasDestroy(canvas);
    }
}

[[nodiscard]] std::string CanvasName(const CanvasState& canvas) noexcept {
    return canvas.path.empty() ? std::string(TextGet(Text::Untitled)) : PathUtf8(canvas.path.filename());
}

void CanvasTitlesUpdate(Canvas canvas) noexcept {
    ZoneScopedN("CanvasTitlesUpdate");
    const CanvasState& state = gAppState.canvases[canvas];
    std::array<char, 512> buffer{};
    const std::string_view title(buffer.data(),
                                 std::format_to_n(buffer.data(), buffer.size() - 1, "{}{} - {}", CanvasName(state),
                                                  state.modified ? "*" : "", MIDORI_NAME)
                                     .out);
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
    const std::string instruction = TextFormat(Text::AskSave, std::string(name).c_str());
    gAppState.modal = true;
    const DialogChoice choice = DialogAsk(DialogAskInfo{
        .title = MIDORI_NAME,
        .instruction = instruction,
        .content = TextGet(Text::AskSaveContent),
        .yes = TextGet(Text::AskSaveYes),
        .no = TextGet(Text::AskSaveNo),
        .window = window,
    });
    gAppState.modal = false;
    return choice;
}

void AppError(Window window, std::string_view instruction, Error error) noexcept {
    ZoneScopedN("AppError");
    const std::string content = TextFormat(Text::ErrorReason, std::string(ErrorMessage(error)).c_str());
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
        AppError(window, TextGet(Text::ErrorFileDialog), path.error());
    }
    return path;
}

// First in the recent files, once.
void RecentFilesAdd(const std::filesystem::path& path) noexcept {
    std::vector<std::filesystem::path>& recent = gAppState.recentFiles;
    std::erase(recent, path);
    recent.insert(recent.begin(), path);
    if (recent.size() > kRecentFilesMax) {
        recent.resize(kRecentFilesMax);
    }
}

[[nodiscard]] std::size_t WindowOpenCount() noexcept {
    std::size_t count = 0;
    for (Window window = 0; window < kWindowMax; ++window) {
        count += WindowGetState(window).has_value();
    }
    return count;
}

// Kept for the next run, which opens its first window there.
void WindowPlacementKeep(Window window) noexcept {
    if (const auto placement = WindowGetPlacement(window)) {
        gAppState.windowPlacement = *placement;
    }
}

// True once the canvas is on disk, false when the user canceled. Failures were shown to the user.
[[nodiscard]] Result<bool> CanvasSaveWithDialogs(Window window, Canvas canvas, bool saveAs) noexcept {
    ZoneScopedN("CanvasSaveWithDialogs");
    const CanvasState& state = gAppState.canvases[canvas];
    if (!saveAs && state.file.env) {
        if (const auto res = CanvasSave(canvas); !res) {
            AppError(window, TextFormat(Text::ErrorSave, CanvasName(state).c_str()), res.error());
            return std::unexpected(res.error());
        }
        CanvasTitlesUpdate(canvas);
        return true;
    }

    std::filesystem::path initial = state.path;
    if (initial.empty()) {
        initial = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(TextGet(Text::Untitled))));
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
        AppError(window, TextFormat(Text::ErrorSave, PathUtf8(path->filename()).c_str()), res.error());
        return std::unexpected(res.error());
    }
    RecentFilesAdd(*path);
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
            RecentFilesAdd(path);
            return {};
        }
    }
    const auto canvas = CanvasOpen(path);
    if (!canvas) {
        AppError(window, TextFormat(Text::ErrorOpen, PathUtf8(path.filename()).c_str()), canvas.error());
        return std::unexpected(canvas.error());
    }
    WindowShowCanvas(window, *canvas);
    RecentFilesAdd(path);
    return {};
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
    return StrokePoint{.x = under.posX,
                       .y = under.posY,
                       .time = gAppState.pointerTime,
                       .pressure = pressure,
                       .tiltX = gAppState.pointerTiltX,
                       .tiltY = gAppState.pointerTiltY};
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
        // The view's level, and the level drawn meanwhile, see TileDrawnLevel. Where the view's level is missing,
        // level 0 too: loading it builds the level.
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
            if (layer == canvas.selection && mip == 0 && !SelectionEmpty(canvas.selectionBounds)) {
                tile.visibleFrame = frame;
            }
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
            if (mip == level || TileDrawn(canvas, tile, level)) {
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
            view.viewport = ViewportZoom(start, amount, amount, gAppState.zoomMin, gAppState.zoomMax);
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

// view.reset: one step on its own, or part of the drag in progress, which goes on from the reset view.
void ViewReset(Window window) noexcept {
    AppState& app = gAppState;
    const CanvasView view = WindowCanvasView(window);
    if (view == kCanvasViewInvalid) {
        return;
    }
    const auto windowState = WindowGetState(window);
    const float dpiScale = windowState ? windowState->dpiScale : 1.0f;
    if (app.navigate.mode == NavigateMode::None) {
        NavigationPush(view, app.canvasViews[view].viewport);
    }
    app.canvasViews[view].viewport = ViewportSnap(ViewportResetZoomRotation(app.canvasViews[view].viewport), dpiScale);
    if (app.navigate.mode != NavigateMode::None) {
        NavigateBegin(app.navigate.view, app.navigate.mode);
    }
}

// Like the reset.
void ViewFlip(Window window, bool vertical) noexcept {
    AppState& app = gAppState;
    const CanvasView view = WindowCanvasView(window);
    if (view == kCanvasViewInvalid) {
        return;
    }
    if (app.navigate.mode == NavigateMode::None) {
        NavigationPush(view, app.canvasViews[view].viewport);
    }
    app.canvasViews[view].viewport = ViewportZoom(app.canvasViews[view].viewport, vertical ? 1.0f : -1.0f,
                                                  vertical ? -1.0f : 1.0f, kViewportZoomMin, kViewportZoomMax);
    if (app.navigate.mode != NavigateMode::None) {
        NavigateBegin(app.navigate.view, app.navigate.mode);
    }
}

// The view fits one of the places carrying every selected tag: `step` past the one holding the view's center, wrapping
// around, or the nearest one when none holds it. Flips and rotation are kept.
void TagIslandShow(Window window, int step) noexcept {
    AppState& app = gAppState;
    const CanvasView view = WindowCanvasView(window);
    const auto windowState = WindowGetState(window);
    if (view == kCanvasViewInvalid || !windowState) {
        return;
    }
    const Viewport& viewport = app.canvasViews[view].viewport;
    const std::vector<TagIsland>& islands = app.canvases[app.canvasViews[view].canvas].tagSelectedIslands;
    const auto first = islands.begin();
    const auto last = islands.end();
    if (first == last) {
        return;
    }
    constexpr auto kSize = static_cast<double>(kTileSize);
    const double x = viewport.posX / kSize;
    const double y = viewport.posY / kSize;
    const auto holding = std::find_if(first, last, [x, y](const TagIsland& island) {
        return x >= island.minX && x < island.maxX + 1 && y >= island.minY && y < island.maxY + 1;
    });
    auto next = first;
    if (holding != last) {
        const auto count = last - first;
        next = first + ((((holding - first) + step) % count) + count) % count;
    } else {
        const auto distance = [x, y](const TagIsland& island) {
            const double dx = ((island.minX + island.maxX + 1) * 0.5) - x;
            const double dy = ((island.minY + island.maxY + 1) * 0.5) - y;
            return (dx * dx) + (dy * dy);
        };
        next = std::min_element(first, last,
                                [&](const TagIsland& a, const TagIsland& b) { return distance(a) < distance(b); });
    }
    const double width = static_cast<double>(next->maxX - next->minX + 1) * kSize;
    const double height = static_cast<double>(next->maxY - next->minY + 1) * kSize;
    // The island's box as the rotated view shows it, with a margin around.
    const double c = std::abs(std::cos(viewport.rot));
    const double s = std::abs(std::sin(viewport.rot));
    const double fit = 0.9
                     * std::min(static_cast<double>(windowState->width) / ((width * c) + (height * s)),
                                static_cast<double>(windowState->height) / ((width * s) + (height * c)));
    const float zoom = std::clamp(static_cast<float>(fit / windowState->dpiScale), app.zoomMin, app.zoomMax);
    Viewport fitted = viewport;
    fitted.posX = (static_cast<double>(next->minX) * kSize) + (width * 0.5);
    fitted.posY = (static_cast<double>(next->minY) * kSize) + (height * 0.5);
    fitted.zoomX = std::copysign(zoom, viewport.zoomX);
    fitted.zoomY = std::copysign(zoom, viewport.zoomY);
    if (app.navigate.mode == NavigateMode::None) {
        NavigationPush(view, viewport);
    }
    app.canvasViews[view].viewport = ViewportSnap(fitted, windowState->dpiScale);
    if (app.navigate.mode != NavigateMode::None) {
        NavigateBegin(app.navigate.view, app.navigate.mode);
    }
}

// Through the navigation history, when no drag is in progress.
void ViewStep(Window window, bool forward) noexcept {
    const CanvasView view = WindowCanvasView(window);
    if (view != kCanvasViewInvalid && gAppState.navigate.mode == NavigateMode::None) {
        NavigationStep(view, forward);
    }
}

// While view.navigate is held: left drag pans, with Ctrl zooms and with Shift rotates, around the view center. Holding
// Alt snaps the zoom (ZoomSnap) and the rotation to 15 degrees. Pressing or releasing Ctrl or Shift mid-drag switches
// mode from where the pointer is. Returns true when the event belongs to navigation. Releases and focus loss are
// handled even when the UI wants them, so a drag never sticks.
[[nodiscard]] bool Navigate(const Event& event, bool uiWants) noexcept {
    ZoneScopedN("Navigate");
    AppState& app = gAppState;
    switch (event.type) {
        case EventType::Key: {
            const NavigateMode mode = NavigateModeFromMods(app.keyMods);
            if (app.navigate.mode != NavigateMode::None && app.navigate.mode != mode) {
                NavigateBegin(app.navigate.view, mode);
            } else if (app.navigate.mode != NavigateMode::None) {
                // Alt pressed or released snaps or frees the view right away.
                NavigateUpdate();
            }
            return false;
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
            // Keys held are released without an event.
            if (!event.windowFocus.focused) {
                app.spaceDown = false;
                NavigateEnd();
                app.keymap.holding = -1;
                app.keymap.repeating = -1;
                app.keymap.pendingLength = 0;
                app.keymap.down.reset();
            }
            return false;
        default:
            return false;
    }
}

#if defined(MIDORI_DEBUG)
// How often the files of the shaders in use and their textures are looked at for changes. Other builds read them again
// from the Tool window.
constexpr std::chrono::milliseconds kBrushCheckInterval{500};
#endif

// A param or curve keeps its value while the new brush has one of the same name and type; the others start from its
// defaults.
void BrushParamsRemap(BrushState& brush, const MdslBrush& from, const MdslBrush& to) noexcept {
    const BrushState before = brush;
    BrushParamsDefault(brush, to);
    for (std::size_t i = 0; i < to.params.size(); ++i) {
        const int j = BrushParamFind(from.params, to.params[i].name);
        if (j >= 0 && from.params[static_cast<std::size_t>(j)].type == to.params[i].type) {
            brush.params[i] = before.params[static_cast<std::size_t>(j)];
        }
    }
    for (std::size_t i = 0; i < to.curves.size(); ++i) {
        if (const int j = BrushCurveFind(from.curves, to.curves[i].name); j >= 0) {
            brush.curves[i] = before.curves[static_cast<std::size_t>(j)];
        }
    }
}

// Reads the image at `path` into the shader's texture `slot`; none when it cannot be read.
void BrushTextureLoad(std::uint32_t shader, std::uint32_t slot, std::filesystem::path path, bool chosen) noexcept {
    BrushTextureState& texture = gAppState.tools.shaders[shader].textures[slot];
    texture = BrushTextureState{.path = std::move(path), .writeTime = {}, .error = {}, .chosen = chosen};
    Result<PlatformImage> image = std::unexpected(Error::ImageDecodeFailed);
    if (!texture.path.empty()) {
        std::error_code ec;
        texture.writeTime = std::filesystem::last_write_time(texture.path, ec);
        image = PlatformImageRead(texture.path);
        if (!image) {
            texture.error = std::format("{}: {}", PathUtf8(texture.path.filename()), ErrorMessage(image.error()));
        }
    }
    if (const auto set = GraphicsBrushTextureSet(shader, slot, image ? &*image : nullptr); !set) {
        texture.error = std::format("{}: {}", PathUtf8(texture.path.filename()), ErrorMessage(set.error()));
    }
}

// A stroke going on paints with it: its dabs hold the params of the brush it began with.
bool BrushShaderStroking(std::uint32_t shader) noexcept {
    const StrokeState& stroke = gAppState.stroke;
    return stroke.canvas != kCanvasInvalid && stroke.shader == shader;
}

// Takes the shader's compiled `pending`, which its tools follow, so only while no stroke uses it. A texture keeps the
// image picked for it while the new brush has one of the same name; the others read the one its source names next to
// its file.
void BrushShaderReplace(std::uint32_t index) noexcept {
    ToolsState& tools = gAppState.tools;
    ToolShader& shader = tools.shaders[index];
    MdslBrush brush = std::move(shader.pending);
    shader.pending = {};
    for (ToolState& tool : tools.library) {
        if (tool.shader == index) {
            BrushParamsRemap(tool.brush, shader.brush, brush);
        }
    }
    const std::filesystem::path folder = shader.path.parent_path();
    const auto before = shader.textures;
    for (std::uint32_t i = 0; i < kMdslTexturesMax; ++i) {
        std::filesystem::path path;
        bool chosen = false;
        if (i < brush.textures.size()) {
            const auto old = std::ranges::find(shader.brush.textures, brush.textures[i].name, &MdslTexture::name);
            const auto j = static_cast<std::size_t>(old - shader.brush.textures.begin());
            if (old != shader.brush.textures.end() && before[j].chosen) {
                path = before[j].path;
                chosen = true;
            } else if (!brush.textures[i].path.empty() && !folder.empty()) {
                path = folder
                     / std::filesystem::path(
                           std::u8string(brush.textures[i].path.begin(), brush.textures[i].path.end()));
            }
        }
        if (path != before[i].path || !before[i].error.empty()) {
            BrushTextureLoad(index, i, path, chosen);
        }
        shader.textures[i].chosen = chosen;
    }
    shader.brush = std::move(brush);
}

// Its pipeline compiles on another thread; BrushShadersUpdate takes it once ready.
void BrushShaderCompile(std::uint32_t index, MdslBrush brush) noexcept {
    ToolShader& shader = gAppState.tools.shaders[index];
    shader.pending = std::move(brush);
    shader.compiling = true;
    shader.ready = false;
    GraphicsBrushCompile(index, shader.pending.hlsl);
}

// The built-in shader as Midori was built with it, another read again from its file: translated on the main thread,
// which is quick.
void BrushShaderLoad(std::uint32_t index) noexcept {
    ToolShader& shader = gAppState.tools.shaders[index];
    if (shader.path.empty()) {
        auto brush = MdslTranslate(round_mdsl);
        MIDORI_ASSERT_ALWAYS(brush.has_value(), "round.mdsl: {}", brush ? "" : brush.error().message);
        BrushShaderCompile(index, std::move(*brush));
        return;
    }
    std::error_code ec;
    shader.writeTime = std::filesystem::last_write_time(shader.path, ec);
    if (auto brush = ToolShaderRead(shader.path, shader.error)) {
        BrushShaderCompile(index, std::move(*brush));
    }
}

// Its file and textures read again, once no stroke uses it.
void BrushShaderReload(std::uint32_t index) noexcept {
    if (BrushShaderStroking(index)) {
        return;
    }
    BrushShaderLoad(index);
    const ToolShader& shader = gAppState.tools.shaders[index];
    for (std::uint32_t i = 0; i < kMdslTexturesMax; ++i) {
        if (!shader.textures[i].path.empty()) {
            BrushTextureLoad(index, i, shader.textures[i].path, shader.textures[i].chosen);
        }
    }
}

// The current tool's shader compiles the first time it is picked. In debug builds a shader in use reads its file and
// textures again when they change. A compiled pipeline is taken once no stroke uses its shader.
void BrushShadersUpdate() noexcept {
    AppState& app = gAppState;
    std::vector<ToolShader>& shaders = app.tools.shaders;
    if (const ToolState& tool = ToolCurrent(app.tools);
        tool.target == ToolTarget::Layer && !GraphicsBrushLoaded(tool.shader) && !shaders[tool.shader].compiling
        && !shaders[tool.shader].ready && shaders[tool.shader].error.empty()) {
        BrushShaderLoad(tool.shader);
    }
#if defined(MIDORI_DEBUG)
    if (const auto now = std::chrono::steady_clock::now(); now - app.brushCheckTime >= kBrushCheckInterval) {
        app.brushCheckTime = now;
        for (std::uint32_t i = 0; i < shaders.size(); ++i) {
            const ToolShader& shader = shaders[i];
            if (shader.path.empty() || BrushShaderStroking(i) || (!GraphicsBrushLoaded(i) && shader.error.empty())) {
                continue;
            }
            std::error_code ec;
            if (const auto time = std::filesystem::last_write_time(shader.path, ec); !ec && time != shader.writeTime) {
                BrushShaderLoad(i);
            }
            for (std::uint32_t t = 0; t < kMdslTexturesMax; ++t) {
                const BrushTextureState& texture = shader.textures[t];
                if (!texture.path.empty() && std::filesystem::last_write_time(texture.path, ec) != texture.writeTime
                    && !ec) {
                    BrushTextureLoad(i, t, texture.path, texture.chosen);
                }
            }
        }
    }
#endif
    std::uint32_t index = 0;
    std::string log;
    switch (GraphicsBrushPoll(index, log)) {
        case GraphicsBrushStatus::Ready:
            shaders[index].ready = shaders[index].compiling;
            shaders[index].compiling = false;
            break;
        case GraphicsBrushStatus::Failed:
            if (shaders[index].compiling) {
                shaders[index].error = std::move(log);
            }
            shaders[index].compiling = false;
            break;
        case GraphicsBrushStatus::None:
        case GraphicsBrushStatus::Compiling:
            break;
    }
    for (std::uint32_t i = 0; i < shaders.size(); ++i) {
        if (shaders[i].ready && !BrushShaderStroking(i)) {
            GraphicsBrushUse(i);
            BrushShaderReplace(i);
            shaders[i].ready = false;
        }
    }
}

// The current tool paints with the shader at `path`, the built-in one when empty; its params keep their values by name.
// Its own shader is read again.
void BrushShaderSet(const std::filesystem::path& path) noexcept {
    ToolsState& tools = gAppState.tools;
    const std::uint32_t index = ToolShaderAdd(tools, path);
    if (index >= kToolShadersMax) {
        MIDORI_LOG_WARN("{} brush files are open already, {} is not opened", kToolShadersMax, PathUtf8(path));
        return;
    }
    ToolState& tool = ToolCurrent(tools);
    if (index == tool.shader) {
        BrushShaderReload(index);
        return;
    }
    BrushParamsRemap(tool.brush, tools.shaders[tool.shader].brush, tools.shaders[index].brush);
    tool.shader = index;
}

[[nodiscard]] Result<void> AppBrushOpen(Window window) noexcept {
    ZoneScopedN("AppBrushOpen");
    const DialogFileInfo info{
        .initial = {},
        .typeName = TextGet(Text::BrushFileType),
        .extension = "mdsl",
        .window = window,
    };
    gAppState.modal = true;
    auto path = DialogFileOpen(info);
    gAppState.modal = false;
    if (!path) {
        AppError(window, TextGet(Text::ErrorFileDialog), path.error());
        return std::unexpected(path.error());
    }
    if (!path->empty()) {
        BrushShaderSet(*path);
    }
    return {};
}

// A picked image stays the texture's until the brush has no texture of that name.
[[nodiscard]] Result<void> AppBrushTextureOpen(Window window, std::uint32_t slot) noexcept {
    ZoneScopedN("AppBrushTextureOpen");
    const std::uint32_t shader = ToolCurrent(gAppState.tools).shader;
    const DialogFileInfo info{
        .initial = gAppState.tools.shaders[shader].textures[slot].path,
        .typeName = TextGet(Text::BrushImageFileType),
        .extension = "png;jpg;jpeg;bmp;tif;tiff;webp",
        .window = window,
    };
    gAppState.modal = true;
    auto path = DialogFileOpen(info);
    gAppState.modal = false;
    if (!path) {
        AppError(window, TextGet(Text::ErrorFileDialog), path.error());
        return std::unexpected(path.error());
    }
    if (!path->empty()) {
        BrushTextureLoad(shader, slot, *path, true);
    }
    return {};
}

// BrushAdjustBegin only starts for a brush that has the param.
[[nodiscard]] float& BrushAdjustTarget(BrushState& brush, BrushAdjustMode mode) noexcept {
    const int param = BrushAdjustParam(ToolSource(gAppState.tools, ToolCurrent(gAppState.tools)).params, mode);
    return brush.params[static_cast<std::size_t>(std::max(param, 0))].value[0];
}

[[nodiscard]] float BrushAdjustValue(const BrushAdjustState& adjust) noexcept {
    if (adjust.mode == BrushAdjustMode::LayerOpacity) {
        return gAppState.canvases[gAppState.canvasViews[adjust.view].canvas].layers[adjust.layer].opacity;
    }
    return BrushAdjustTarget(ToolCurrent(gAppState.tools).brush, adjust.mode);
}

void BrushAdjustValueSet(const BrushAdjustState& adjust, float value) noexcept {
    if (adjust.mode != BrushAdjustMode::LayerOpacity) {
        BrushAdjustTarget(ToolCurrent(gAppState.tools).brush, adjust.mode) = value;
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
    if (adjust.mode == BrushAdjustMode::LayerOpacity) {
        value = std::clamp(value, 0.0f, 1.0f);
    } else {
        const std::vector<MdslParam>& params = ToolSource(gAppState.tools, ToolCurrent(gAppState.tools)).params;
        const MdslParam& param = params[static_cast<std::size_t>(BrushAdjustParam(params, adjust.mode))];
        value = std::clamp(value, param.min, param.max);
    }
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

// brush.radius, brush.opacity and brush.hardness set the current tool's brush params named size, opacity and hardness,
// layer.opacity the active layer's
// opacity, from the pointer's distance to a center placed so the current value starts under the pointer. Not while
// navigating or stroking.
void BrushAdjustBegin(Window window, BrushAdjustMode mode) noexcept {
    ZoneScopedN("BrushAdjustBegin");
    AppState& app = gAppState;
    if (app.brushAdjust.mode != BrushAdjustMode::None || app.spaceDown || app.navigate.mode != NavigateMode::None
        || (app.stroke.canvas != kCanvasInvalid && !app.stroke.ended)) {
        return;
    }
    const CanvasView view = WindowCanvasView(window);
    const auto windowState = WindowGetState(window);
    if (view == kCanvasViewInvalid || !windowState) {
        return;
    }
    const CanvasState& canvas = app.canvases[app.canvasViews[view].canvas];
    if (mode == BrushAdjustMode::LayerOpacity
        && (canvas.activeLayer == kLayerInvalid || canvas.merge.result != kLayerInvalid)) {
        return;
    }
    if (mode != BrushAdjustMode::LayerOpacity
        && BrushAdjustParam(ToolSource(app.tools, ToolCurrent(app.tools)).params, mode) < 0) {
        return;
    }
    BrushAdjustState& adjust = app.brushAdjust;
    adjust = BrushAdjustState{.layer = canvas.activeLayer, .view = view, .mode = mode};
    adjust.startValue = BrushAdjustValue(adjust);
    adjust.value = adjust.startValue;
    adjust.distance = adjust.value * BrushAdjustPixelsPerUnit(adjust, windowState->dpiScale);
    adjust.centerX = app.pointerX - adjust.distance;
    adjust.centerY = app.pointerY;
}

// Holding Shift moves the value 10 times slower and Alt snaps it to powers of two, a layer opacity to multiples of 5%,
// together or not. Left click or Enter keeps the value, right click or Escape restores it. While adjusting, every
// input event belongs to it.
[[nodiscard]] bool BrushAdjust(const Event& event) noexcept {
    ZoneScopedN("BrushAdjust");
    AppState& app = gAppState;
    BrushAdjustState& adjust = app.brushAdjust;
    if (adjust.mode == BrushAdjustMode::None) {
        return false;
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

// Around a transform's handles, in DIPs.
constexpr float kTransformHandleReachDips = 8.0f;
constexpr double kTransformRotationSnap = std::numbers::pi / 12.0;
// Keeps a scale from flattening the box, which could not be turned back.
constexpr double kTransformScaleMin = 1e-3;
// Over the lifted box, the smallest divisor of a distortion against the largest: what one texel may grow to.
constexpr double kTransformDivisorRatioMin = 1.0 / 64.0;

struct CanvasPoint {
    double x;
    double y;
};

// The grid positions a transform of what is under `bounds` lifts its tiles from.
[[nodiscard]] std::size_t TransformTileCount(const SelectionBounds& bounds) noexcept {
    const auto tileOf = [](double pos) {
        return static_cast<std::int64_t>(std::floor(pos / kTileSize));
    };
    return static_cast<std::size_t>((tileOf(bounds.maxX) - tileOf(bounds.minX) + 1)
                                    * (tileOf(bounds.maxY) - tileOf(bounds.minY) + 1));
}

// `a` after `b`.
[[nodiscard]] StrokeTransform TransformCompose(const StrokeTransform& a, const StrokeTransform& b) noexcept {
    return StrokeTransform{
        .xx = (a.xx * b.xx) + (a.xy * b.yx) + (a.x * b.wx),
        .xy = (a.xx * b.xy) + (a.xy * b.yy) + (a.x * b.wy),
        .yx = (a.yx * b.xx) + (a.yy * b.yx) + (a.y * b.wx),
        .yy = (a.yx * b.xy) + (a.yy * b.yy) + (a.y * b.wy),
        .x = (a.xx * b.x) + (a.xy * b.y) + (a.x * b.w),
        .y = (a.yx * b.x) + (a.yy * b.y) + (a.y * b.w),
        .wx = (a.wx * b.xx) + (a.wy * b.yx) + (a.w * b.wx),
        .wy = (a.wx * b.xy) + (a.wy * b.yy) + (a.w * b.wy),
        .w = (a.wx * b.x) + (a.wy * b.y) + (a.w * b.w),
    };
}

// The adjugate over the determinant: a point the transform reaches maps back with a divisor above 0.
[[nodiscard]] StrokeTransform TransformInverse(const StrokeTransform& t) noexcept {
    StrokeTransform inverse{
        .xx = (t.yy * t.w) - (t.y * t.wy),
        .xy = (t.x * t.wy) - (t.xy * t.w),
        .yx = (t.y * t.wx) - (t.yx * t.w),
        .yy = (t.xx * t.w) - (t.x * t.wx),
        .x = (t.xy * t.y) - (t.x * t.yy),
        .y = (t.x * t.yx) - (t.xx * t.y),
        .wx = (t.yx * t.wy) - (t.yy * t.wx),
        .wy = (t.xy * t.wx) - (t.xx * t.wy),
        .w = (t.xx * t.yy) - (t.xy * t.yx),
    };
    const double det = (t.xx * inverse.xx) + (t.xy * inverse.yx) + (t.x * inverse.wx);
    for (double* value : {&inverse.xx, &inverse.xy, &inverse.yx, &inverse.yy, &inverse.x, &inverse.y, &inverse.wx,
                          &inverse.wy, &inverse.w}) {
        *value /= det;
    }
    return inverse;
}

// The source box onto the quad of `to`, clockwise from the top left corner (Heckbert's square to quad).
[[nodiscard]] StrokeTransform TransformQuad(const SelectionBounds& source,
                                            const std::array<CanvasPoint, 4>& to) noexcept {
    const auto [x0, y0] = to[0];
    const auto [x1, y1] = to[1];
    const auto [x2, y2] = to[2];
    const auto [x3, y3] = to[3];
    const double dx1 = x1 - x2;
    const double dy1 = y1 - y2;
    const double dx2 = x3 - x2;
    const double dy2 = y3 - y2;
    const double dx3 = x0 - x1 + x2 - x3;
    const double dy3 = y0 - y1 + y2 - y3;
    const double det = (dx1 * dy2) - (dx2 * dy1);
    const double g = ((dx3 * dy2) - (dx2 * dy3)) / det;
    const double h = ((dx1 * dy3) - (dx3 * dy1)) / det;
    const StrokeTransform square{
        .xx = x1 - x0 + (g * x1),
        .xy = x3 - x0 + (h * x3),
        .yx = y1 - y0 + (g * y1),
        .yy = y3 - y0 + (h * y3),
        .x = x0,
        .y = y0,
        .wx = g,
        .wy = h,
        .w = 1.0,
    };
    const double width = source.maxX - source.minX;
    const double height = source.maxY - source.minY;
    return TransformCompose(square, StrokeTransform{
                                        .xx = 1.0 / width,
                                        .yy = 1.0 / height,
                                        .x = -source.minX / width,
                                        .y = -source.minY / height,
                                    });
}

// The divisor stays above 0 over the box a texel around, and no texel grows past kTransformDivisorRatioMin: a quad
// folded or reaching the horizon could not be lifted.
[[nodiscard]] bool TransformLiftable(const StrokeTransform& transform, const SelectionBounds& source) noexcept {
    double min = std::numeric_limits<double>::max();
    double max = 0.0;
    for (const double x : {source.minX - 1.0, source.maxX + 1.0}) {
        for (const double y : {source.minY - 1.0, source.maxY + 1.0}) {
            const double w = (transform.wx * x) + (transform.wy * y) + transform.w;
            if (!std::isfinite(w) || w <= 0.0) {
                return false;
            }
            min = std::min(min, w);
            max = std::max(max, w);
        }
    }
    return min >= max * kTransformDivisorRatioMin;
}

[[nodiscard]] StrokeTransform TransformMatrix(const TransformParams& params, const SelectionBounds& source) noexcept {
    const double centerX = (source.minX + source.maxX) * 0.5;
    const double centerY = (source.minY + source.maxY) * 0.5;
    const double c = std::cos(params.rotation);
    const double s = std::sin(params.rotation);
    // R(rotation) S(scale).
    StrokeTransform transform{
        .xx = c * params.scaleX,
        .xy = -s * params.scaleY,
        .yx = s * params.scaleX,
        .yy = c * params.scaleY,
    };
    transform.x = centerX + params.offsetX - ((transform.xx * centerX) + (transform.xy * centerY));
    transform.y = centerY + params.offsetY - ((transform.yx * centerX) + (transform.yy * centerY));
    return params.distort == StrokeTransform{} ? transform : TransformCompose(transform, params.distort);
}

// Where the transform puts the point of the source box half its size from the center along each axis times `handle`.
[[nodiscard]] CanvasPoint TransformPoint(const StrokeTransform& transform, const SelectionBounds& source,
                                         double handleX, double handleY) noexcept {
    const double x = ((source.minX + source.maxX) * 0.5) + (handleX * (source.maxX - source.minX) * 0.5);
    const double y = ((source.minY + source.maxY) * 0.5) + (handleY * (source.maxY - source.minY) * 0.5);
    const StrokeTransformPoint to = StrokeTransformApply(transform, x, y);
    return CanvasPoint{.x = to.x, .y = to.y};
}

struct TransformHit {
    TransformDrag drag;
    std::int8_t handleX;
    std::int8_t handleY;
};

// A handle within reach of the pointer scales, or with Alt moves alone; else inside the box moves and outside turns.
[[nodiscard]] TransformHit TransformHitTest(const TransformBox& box, CanvasPoint point) noexcept {
    const double reach =
        kTransformHandleReachDips / std::max(std::abs(gAppState.canvasViews[box.view].viewport.zoomX), 1e-6f);
    const bool alt = HasAnyFlag(gAppState.keyMods, KeyMod::Alt);
    for (std::int8_t y = -1; y <= 1; ++y) {
        for (std::int8_t x = -1; x <= 1; ++x) {
            if (x == 0 && y == 0) {
                continue;
            }
            const CanvasPoint handle = TransformPoint(box.transform, box.source, x, y);
            if (std::hypot(point.x - handle.x, point.y - handle.y) <= reach) {
                return TransformHit{
                    .drag = alt ? TransformDrag::Distort : TransformDrag::Scale, .handleX = x, .handleY = y};
            }
        }
    }
    const StrokeTransform inverse = TransformInverse(box.transform);
    const double w = (inverse.wx * point.x) + (inverse.wy * point.y) + inverse.w;
    const StrokeTransformPoint from = StrokeTransformApply(inverse, point.x, point.y);
    const bool inside = w > 0.0 && from.x >= box.source.minX && from.x <= box.source.maxX && from.y >= box.source.minY
                     && from.y <= box.source.maxY;
    return TransformHit{.drag = inside ? TransformDrag::Move : TransformDrag::Rotate, .handleX = 0, .handleY = 0};
}

// The resize arrow closest to the direction from the box's center to the handle, as the view shows it.
[[nodiscard]] WindowCursor TransformResizeCursor(const TransformBox& box, std::int8_t handleX,
                                                 std::int8_t handleY) noexcept {
    const Viewport& viewport = gAppState.canvasViews[box.view].viewport;
    const CanvasPoint center = TransformPoint(box.transform, box.source, 0.0, 0.0);
    const CanvasPoint handle = TransformPoint(box.transform, box.source, handleX, handleY);
    const double dx = handle.x - center.x;
    const double dy = handle.y - center.y;
    const double c = std::cos(static_cast<double>(viewport.rot));
    const double s = std::sin(static_cast<double>(viewport.rot));
    const double screenX = ((dx * c) - (dy * s)) * viewport.zoomX;
    const double screenY = ((dx * s) + (dy * c)) * viewport.zoomY;
    double angle = std::atan2(screenY, screenX);
    if (angle < 0.0) {
        angle += std::numbers::pi;
    }
    constexpr std::array kCursors = {WindowCursor::ResizeEW, WindowCursor::ResizeNWSE, WindowCursor::ResizeNS,
                                     WindowCursor::ResizeNESW};
    return kCursors[static_cast<std::size_t>(std::lround(angle / (std::numbers::pi / 4.0))) % kCursors.size()];
}

// The drag's, or what a press there would start.
[[nodiscard]] WindowCursor TransformCursor(const TransformBox& box) noexcept {
    const TransformState& transform = gAppState.transform;
    TransformHit hit{.drag = TransformDrag::None, .handleX = 0, .handleY = 0};
    if (AppTransforming()) {
        hit = TransformHit{.drag = transform.drag, .handleX = transform.handleX, .handleY = transform.handleY};
    }
    if (hit.drag == TransformDrag::None) {
        const auto point = CanvasPointUnderPointer(box.view, 1.0f);
        if (!point) {
            return WindowCursor::Arrow;
        }
        hit = TransformHitTest(box, CanvasPoint{.x = point->x, .y = point->y});
    }
    switch (hit.drag) {
        case TransformDrag::Move:
            return WindowCursor::Move;
        case TransformDrag::Scale:
        case TransformDrag::Distort:
            return TransformResizeCursor(box, hit.handleX, hit.handleY);
        case TransformDrag::Rotate:
        case TransformDrag::None:
            break;
    }
    return WindowCursor::Arrow;
}

// The drag from its start to `point`. A move snaps to whole texels. Scaling keeps the opposite handle in place, and
// with Shift on a corner the box's proportions; Shift snaps a turn to 15 degrees. A handle moved alone takes its
// corner, or the two of its edge, with the pointer while the others stay.
void TransformDragTo(CanvasPoint point) noexcept {
    ZoneScopedN("TransformDragTo");
    AppState& app = gAppState;
    TransformState& transform = app.transform;
    const SelectionBounds& source = app.stroke.selectionBefore;
    const TransformParams& start = transform.start;
    const bool shift = HasAnyFlag(app.keyMods, KeyMod::Shift);
    const double width = source.maxX - source.minX;
    const double height = source.maxY - source.minY;
    const double hx = transform.handleX;
    const double hy = transform.handleY;
    TransformParams flat = start;
    flat.distort = {};
    const StrokeTransform affine = TransformMatrix(flat, source);
    // The pointers taken out of the distortion, so the handles and the center are where they would be without it.
    const StrokeTransform unproject = TransformCompose(affine, TransformInverse(TransformMatrix(start, source)));
    const StrokeTransformPoint flatPoint = StrokeTransformApply(unproject, point.x, point.y);
    const StrokeTransformPoint flatStart =
        StrokeTransformApply(unproject, transform.pointerStartX, transform.pointerStartY);
    const CanvasPoint center = TransformPoint(affine, source, 0.0, 0.0);
    const double c = std::cos(start.rotation);
    const double s = std::sin(start.rotation);
    // Turned back into the box's axes.
    const auto unturn = [&](double x, double y) {
        return CanvasPoint{.x = (c * x) + (s * y), .y = (c * y) - (s * x)};
    };
    TransformParams params = start;
    switch (transform.drag) {
        case TransformDrag::Move:
            // Whole texels: a move alone then lands every texel on one, with nothing resampled.
            params.offsetX = std::round(start.offsetX + point.x - transform.pointerStartX);
            params.offsetY = std::round(start.offsetY + point.y - transform.pointerStartY);
            break;
        case TransformDrag::Rotate:
            params.rotation = start.rotation + std::atan2(flatPoint.y - center.y, flatPoint.x - center.x)
                            - std::atan2(flatStart.y - center.y, flatStart.x - center.x);
            if (shift) {
                params.rotation = std::round(params.rotation / kTransformRotationSnap) * kTransformRotationSnap;
            }
            break;
        case TransformDrag::Scale: {
            const CanvasPoint anchor = TransformPoint(affine, source, -hx, -hy);
            const CanvasPoint turned = unturn(flatPoint.x - anchor.x, flatPoint.y - anchor.y);
            const double dx = turned.x;
            const double dy = turned.y;
            if (hx != 0.0) {
                params.scaleX = dx / (hx * width);
            }
            if (hy != 0.0) {
                params.scaleY = dy / (hy * height);
            }
            if (shift && hx != 0.0 && hy != 0.0) {
                const double vx = start.scaleX * hx * width;
                const double vy = start.scaleY * hy * height;
                const double along = ((dx * vx) + (dy * vy)) / ((vx * vx) + (vy * vy));
                params.scaleX = start.scaleX * along;
                params.scaleY = start.scaleY * along;
            }
            const auto clamp = [](double scale) {
                return std::abs(scale) < kTransformScaleMin ? std::copysign(kTransformScaleMin, scale) : scale;
            };
            params.scaleX = clamp(params.scaleX);
            params.scaleY = clamp(params.scaleY);
            // The center is half the new box from the anchor.
            const double halfX = params.scaleX * hx * width * 0.5;
            const double halfY = params.scaleY * hy * height * 0.5;
            params.offsetX = anchor.x + (c * halfX) - (s * halfY) - ((source.minX + source.maxX) * 0.5);
            params.offsetY = anchor.y + (s * halfX) + (c * halfY) - ((source.minY + source.maxY) * 0.5);
            break;
        }
        case TransformDrag::Distort: {
            constexpr std::array<std::array<std::int8_t, 2>, 4> kCorners = {{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
            const StrokeTransform before = TransformMatrix(start, source);
            std::array<CanvasPoint, 4> corners{};
            for (std::size_t i = 0; i < kCorners.size(); ++i) {
                corners[i] = TransformPoint(before, source, kCorners[i][0], kCorners[i][1]);
                if ((transform.handleX == 0 || kCorners[i][0] == transform.handleX)
                    && (transform.handleY == 0 || kCorners[i][1] == transform.handleY)) {
                    corners[i].x += point.x - transform.pointerStartX;
                    corners[i].y += point.y - transform.pointerStartY;
                }
            }
            const StrokeTransform quad = TransformQuad(source, corners);
            if (!TransformLiftable(quad, source)) {
                return;
            }
            params.distort = TransformCompose(TransformInverse(affine), quad);
            break;
        }
        case TransformDrag::None:
            return;
    }
    transform.params = params;
    const StrokeTransform matrix = TransformMatrix(params, source);
    CanvasState& canvas = app.canvases[app.stroke.canvas];
    const bool wasModified = canvas.modified;
    StrokeTransformTo(matrix);
    canvas.selectionBounds = StrokeTransformBounds(matrix, source);
    CanvasEdited(canvas.id, wasModified);
}

// With Ctrl held, a left press over a view showing the selection's box starts transforming it.
[[nodiscard]] bool TransformHoldBegin(const Event& event, bool uiWants) noexcept {
    if (event.type != EventType::Button || event.button.button != Button::Left
        || event.button.inputState != InputState::Press || uiWants || AppTransforming()
        || !AppTransformBox(event.button.window)) {
        return false;
    }
    if (!AppTransformMode(event.button.window) || !AppTransforming()) {
        return false;
    }
    gAppState.transform.held = true;
    return true;
}

// A transform begun with Ctrl held is applied once Ctrl is up and no drag is left.
void TransformHoldEnd() noexcept {
    const TransformState& transform = gAppState.transform;
    if (AppTransforming() && transform.held && transform.drag == TransformDrag::None
        && !HasAnyFlag(gAppState.keyMods, KeyMod::Ctrl)) {
        AppTransformEnd(true);
    }
}

// While a selection is transformed, left drags over its view go to the handles, see TransformHitTest. Returns true
// when the event belongs to the transform.
[[nodiscard]] bool TransformEvent(const Event& event, bool uiWants) noexcept {
    if (!AppTransforming() && !TransformHoldBegin(event, uiWants)) {
        return false;
    }
    ZoneScopedN("TransformEvent");
    AppState& app = gAppState;
    TransformState& transform = app.transform;
    const CanvasView view = app.stroke.view;
    switch (event.type) {
        case EventType::Button: {
            if (event.button.button != Button::Left) {
                return false;
            }
            if (event.button.inputState == InputState::Release) {
                const bool dragging = std::exchange(transform.drag, TransformDrag::None) != TransformDrag::None;
                TransformHoldEnd();
                return dragging;
            }
            if (event.button.inputState != InputState::Press || uiWants
                || WindowCanvasView(event.button.window) != view) {
                return false;
            }
            const auto point = CanvasPointUnderPointer(view, 1.0f);
            if (!point) {
                return false;
            }
            const TransformHit hit = TransformHitTest(
                TransformBox{.transform = app.stroke.transform, .source = app.stroke.selectionBefore, .view = view},
                CanvasPoint{.x = point->x, .y = point->y});
            transform.drag = hit.drag;
            transform.handleX = hit.handleX;
            transform.handleY = hit.handleY;
            transform.start = transform.params;
            transform.pointerStartX = point->x;
            transform.pointerStartY = point->y;
            return true;
        }
        case EventType::Pointer: {
            if (transform.drag == TransformDrag::None || app.canvasViews[view].window != event.pointer.window) {
                return false;
            }
            if (const auto point = CanvasPointUnderPointer(view, 1.0f)) {
                TransformDragTo(CanvasPoint{.x = point->x, .y = point->y});
            }
            return true;
        }
        case EventType::Key:
            TransformHoldEnd();
            return false;
        case EventType::WindowFocus:
            if (!event.windowFocus.focused) {
                transform.drag = TransformDrag::None;
                if (transform.held) {
                    AppTransformEnd(true);
                }
            }
            return false;
        default:
            return false;
    }
}

// The UI's cursor over the UI. Over the canvas: navigation's while Space is held, the transform's over its box, a
// crosshair with a Color tool, none with a Layer tool, the brush outline showing where the paint or erase lands, else
// the arrow. Zoom drags right or up, like the NE-SW arrow; rotation has no system cursor.
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
    if (const auto box = AppTransformBox(window)) {
        return TransformCursor(*box);
    }
    if (app.eyedropper.view != kCanvasViewInvalid || (app.stroke.canvas == kCanvasInvalid && AppEyedropperActive())) {
        return WindowCursor::Crosshair;
    }
    return ToolCurrent(app.tools).target == ToolTarget::Layer ? WindowCursor::None : WindowCursor::Arrow;
}

// The current tool's brush under the pointer, over the canvas only: not over the UI, while navigating, or while
// adjusting the brush, which shows its own. While painting or erasing, the radius the stroke paints at now.
[[nodiscard]] BrushOutlineState BrushOutline(Window window) noexcept {
    ZoneScopedN("BrushOutline");
    const AppState& app = gAppState;
    const CanvasView view = WindowCanvasView(window);
    const auto windowState = WindowGetState(window);
    // A selection has no brush.
    if (view == kCanvasViewInvalid || !windowState || !windowState->hovered || UiCursor(window) || app.spaceDown
        || app.navigate.mode != NavigateMode::None || app.brushAdjust.mode != BrushAdjustMode::None
        || ((ToolCurrent(app.tools).target != ToolTarget::Layer || AppEyedropperActive())
            && app.stroke.canvas == kCanvasInvalid)
        || app.eyedropper.view != kCanvasViewInvalid
        || (app.stroke.canvas != kCanvasInvalid && app.stroke.shape != StrokeShape::Brush) || AppTransformBox(window)) {
        return {};
    }
    const float scale = std::abs(app.canvasViews[view].viewport.zoomX) * windowState->dpiScale;
    const StrokeState& stroke = app.stroke;
    const bool painting = stroke.canvas != kCanvasInvalid && !stroke.ended && stroke.view == view;
    float radius = 0.0f;
    if (painting) {
        radius =
            StrokeStampAt(app.tools.shaders[stroke.shader].brush, stroke.brush, stroke.curves, stroke.inputs).radius;
    } else {
        const BrushState& brush = ToolCurrent(app.tools).brush;
        std::array<float, kMdslCurveFloats> curves{};
        BrushCurvesBake(brush, curves);
        radius = StrokeStampAt(ToolSource(app.tools, ToolCurrent(app.tools)), brush, curves,
                               MdslDab{
                                   .pressure = 1.0f,
                                   .tiltX = app.pointerTiltX,
                                   .tiltY = app.pointerTiltY,
                                   .velocityX = 0.0f,
                                   .velocityY = 0.0f,
                                   .directionX = 1.0f,
                                   .directionY = 0.0f,
                                   .random = 0.5f,
                                   .along = 0.0f,
                                   .time = 0.0f,
                               })
                     .radius;
    }
    return BrushOutlineState{
        .x = app.pointerX,
        .y = app.pointerY,
        .radius = radius * scale,
        .lineWidth = std::max(1.0f, std::round(windowState->dpiScale)),
        .shown = true,
    };
}

// The color moves to the front of the recent ones.
void ColorRecentPush(ToolsState& tools, Color::Srgb color) noexcept {
    std::erase_if(tools.recentColors, [color](const Color::Srgb& recent) {
        return recent.r == color.r && recent.g == color.g && recent.b == color.b && recent.a == color.a;
    });
    tools.recentColors.insert(tools.recentColors.begin(), color);
    if (tools.recentColors.size() > kRecentColorsMax) {
        tools.recentColors.resize(kRecentColorsMax);
    }
}

[[nodiscard]] StrokeBeginInfo StrokeBeginInfoFromTool(CanvasView view, StrokePoint point) noexcept {
    const ToolsState& tools = gAppState.tools;
    const Layer layer = gAppState.canvases[gAppState.canvasViews[view].canvas].activeLayer;
    // Erasing creates no tile where there is none.
    const bool erase = ToolCurrent(tools).blendMode == BlendMode::Erase;
    constexpr std::array kShapes = {StrokeShape::Brush, StrokeShape::FillRect, StrokeShape::FillLasso};
    return StrokeBeginInfo{
        .brush = ToolCurrent(tools).brush,
        .color = erase ? Color::Srgb{.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f} : tools.colors[tools.colorSlot],
        .point = point,
        .layer = layer,
        .view = view,
        .mode = erase ? StrokeMode::Erase : StrokeMode::Paint,
        .blendMode = erase ? BlendMode::Normal : ToolCurrent(tools).blendMode,
        .shape = kShapes[std::to_underlying(ToolCurrent(tools).shape)],
        .shader = ToolCurrent(tools).shader,
    };
}

// The tool's operation, or the held selection.op binding's (Shift adds, Alt subtracts, Shift+Alt intersects by
// default).
[[nodiscard]] SelectionOp SelectionOpHeld() noexcept {
    SelectionOp op = ToolCurrent(gAppState.tools).selectionOp;
    const KeymapState& keymap = gAppState.keymap;
    if (const std::int32_t held = KeymapHeld(keymap, ActionId::SelectionOp, ActionContext::Canvas, gAppState.keyMods);
        held >= 0 && keymap.bindings[static_cast<std::size_t>(held)].arg < std::to_underlying(SelectionOp::Count_)) {
        op = static_cast<SelectionOp>(keymap.bindings[static_cast<std::size_t>(held)].arg);
    }
    return op;
}

// A rectangle into the canvas's selection by SelectionOpHeld. Nothing when no layer id is left for the selection's
// mask.
[[nodiscard]] std::optional<StrokeBeginInfo> SelectionBeginInfo(CanvasView view, StrokePoint point,
                                                                StrokeShape shape) noexcept {
    CanvasState& canvas = gAppState.canvases[gAppState.canvasViews[view].canvas];
    const Layer layer = LayerSelectionTake(canvas);
    if (layer == kLayerInvalid) {
        return std::nullopt;
    }
    constexpr std::array<StrokeMode, std::to_underlying(SelectionOp::Count_)> kModes = {
        StrokeMode::Replace, StrokeMode::Paint, StrokeMode::Erase, StrokeMode::Intersect, StrokeMode::Exclude,
    };
    const StrokeMode mode = kModes[std::to_underlying(SelectionOpHeld())];
    return StrokeBeginInfo{
        .brush = {},
        .color = Color::Srgb{.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f},
        .point = point,
        .layer = layer,
        .view = view,
        .mode = mode,
        .blendMode = BlendMode::Normal,
        .shape = shape,
    };
}

// While a stroke of the selection goes on, its canvas's bounds follow the rectangle, or the lasso's box: they are it in
// place of the selection, grow to it when adding or excluding, shrink to it when intersecting, and stay as they were
// when removing, which may leave them too wide. A lasso of fewer than 3 points selects nothing.
void SelectionBoundsFollow() noexcept {
    const StrokeState& stroke = gAppState.stroke;
    CanvasState& canvas = gAppState.canvases[stroke.canvas];
    if (stroke.layer != canvas.selection) {
        return;
    }
    SelectionBounds rect{
        .minX = std::min(stroke.first.x, stroke.last.x),
        .minY = std::min(stroke.first.y, stroke.last.y),
        .maxX = std::max(stroke.first.x, stroke.last.x),
        .maxY = std::max(stroke.first.y, stroke.last.y),
    };
    if (stroke.shape == StrokeShape::Lasso) {
        rect = {};
        if (stroke.dabs.size() >= 3) {
            const double originX = static_cast<double>(stroke.origin.x) * kTileSize;
            const double originY = static_cast<double>(stroke.origin.y) * kTileSize;
            rect = SelectionBounds{
                .minX = originX + std::ranges::min(stroke.dabs, {}, &StrokeDab::x).x,
                .minY = originY + std::ranges::min(stroke.dabs, {}, &StrokeDab::y).y,
                .maxX = originX + std::ranges::max(stroke.dabs, {}, &StrokeDab::x).x,
                .maxY = originY + std::ranges::max(stroke.dabs, {}, &StrokeDab::y).y,
            };
        }
    }
    const SelectionBounds& before = stroke.selectionBefore;
    switch (stroke.mode) {
        case StrokeMode::Replace:
            canvas.selectionBounds = rect;
            break;
        case StrokeMode::Intersect:
            canvas.selectionBounds = SelectionBounds{
                .minX = std::max(before.minX, rect.minX),
                .minY = std::max(before.minY, rect.minY),
                .maxX = std::min(before.maxX, rect.maxX),
                .maxY = std::min(before.maxY, rect.maxY),
            };
            if (SelectionEmpty(canvas.selectionBounds)) {
                canvas.selectionBounds = {};
            }
            break;
        case StrokeMode::Paint:
        case StrokeMode::Exclude:
            if (SelectionEmpty(before) || SelectionEmpty(rect)) {
                canvas.selectionBounds = SelectionEmpty(rect) ? before : rect;
            } else {
                canvas.selectionBounds = SelectionBounds{
                    .minX = std::min(before.minX, rect.minX),
                    .minY = std::min(before.minY, rect.minY),
                    .maxX = std::max(before.maxX, rect.maxX),
                    .maxY = std::max(before.maxY, rect.maxY),
                };
            }
            break;
        case StrokeMode::Erase:
            canvas.selectionBounds = before;
            break;
    }
}

// One tile of the pasted image into an upload slot's memory, RGBA8 rows of the tile; transparent around the image.
// False when the image is transparent all over it.
[[nodiscard]] bool PasteTileFill(const PasteState& paste, TileGridPos gridPos, std::byte* memory) noexcept {
    constexpr std::int64_t kSize = kTileSize;
    const std::int64_t tileX = std::int64_t{gridPos.x} * kSize;
    const std::int64_t tileY = std::int64_t{gridPos.y} * kSize;
    const std::int64_t left = std::max(paste.x, tileX);
    const std::int64_t right = std::min(paste.x + paste.width, tileX + kSize);
    const std::int64_t top = std::max(paste.y, tileY);
    const std::int64_t bottom = std::min(paste.y + paste.height, tileY + kSize);
    std::memset(memory, 0, static_cast<std::size_t>(kSize * kSize * 4));
    bool opaque = false;
    for (std::int64_t y = top; y < bottom; ++y) {
        const std::byte* from = paste.pixels.data() + ((((y - paste.y) * paste.width) + (left - paste.x)) * 4);
        std::memcpy(memory + ((((y - tileY) * kSize) + (left - tileX)) * 4), from,
                    static_cast<std::size_t>((right - left) * 4));
        for (std::int64_t i = 3; i < (right - left) * 4 && !opaque; i += 4) {
            opaque = from[i] != std::byte{0};
        }
    }
    return opaque;
}

// edit.paste: the clipboard's image into a new layer above the active one, at 100% centered on the view. PasteUpdate
// uploads it, selects it and opens a transform.
void AppPaste(Window window) noexcept {
    ZoneScopedN("AppPaste");
    if (ActionBlocked(window, ActionId::EditPaste, 0) != Text::Count_) {
        return;
    }
    auto image = PlatformClipboardImage();
    if (!image) {
        MIDORI_LOG_WARN("paste: {}", ErrorMessage(image.error()));
        return;
    }
    const CanvasView view = WindowCanvasView(window);
    CanvasState& canvas = gAppState.canvases[gAppState.canvasViews[view].canvas];
    const Layer before = canvas.activeLayer;
    AppLayerNew(window, false);
    if (canvas.activeLayer == before) {
        return;
    }
    // Centered on the view, unless it is this app's last copy and where it came from shows, as Photoshop does.
    const Viewport& viewport = gAppState.canvasViews[view].viewport;
    std::int64_t x = std::llround(viewport.posX - (image->width * 0.5));
    std::int64_t y = std::llround(viewport.posY - (image->height * 0.5));
    if (const CopyState& copy = gAppState.copy; copy.copied && copy.sequence == PlatformClipboardSequence()
                                                && copy.width == image->width && copy.height == image->height) {
        // The view's bounds on the canvas, at any rotation.
        const auto windowState = WindowGetState(window);
        const double halfWidth = windowState ? windowState->width / windowState->dpiScale * 0.5 : 0.0;
        const double halfHeight = windowState ? windowState->height / windowState->dpiScale * 0.5 : 0.0;
        const double c = std::abs(std::cos(static_cast<double>(viewport.rot)));
        const double s = std::abs(std::sin(static_cast<double>(viewport.rot)));
        const double reachX = ((c * halfWidth) + (s * halfHeight)) / std::abs(viewport.zoomX);
        const double reachY = ((s * halfWidth) + (c * halfHeight)) / std::abs(viewport.zoomY);
        const bool shown =
            static_cast<double>(copy.x)<viewport.posX + reachX&& static_cast<double>(copy.x + copy.width)> viewport.posX
                - reachX
            && static_cast<double>(copy.y)<viewport.posY + reachY&& static_cast<double>(copy.y + copy.height)> viewport
                       .posY
                   - reachY;
        if (shown) {
            x = copy.x;
            y = copy.y;
        }
    }
    PasteState paste{
        .pixels = std::move(image->pixels),
        .remaining = {},
        .x = x,
        .y = y,
        .width = image->width,
        .height = image->height,
        .window = window,
        .canvas = canvas.id,
        .layer = canvas.activeLayer,
        .selected = false,
    };
    const auto tileOf = [](std::int64_t pos) {
        return static_cast<TileAxis>(pos >= 0 ? pos / kTileSize : ((pos + 1) / kTileSize) - 1);
    };
    for (TileAxis tileY = tileOf(paste.y); tileY <= tileOf(paste.y + paste.height - 1); ++tileY) {
        for (TileAxis tileX = tileOf(paste.x); tileX <= tileOf(paste.x + paste.width - 1); ++tileX) {
            paste.remaining.push_back(TileGridPos{.x = tileX, .y = tileY});
        }
    }
    gAppState.paste = std::move(paste);
}

// The paste in progress, once per update before streaming places the uploads: its tiles go to the free upload slots as
// new Modified tiles of its layer, Loaded once copied; once all are, the selection becomes its rectangle; once that
// stroke is done, a transform opens on it. Dropped when its window, canvas or layer goes.
void PasteUpdate() noexcept {
    PasteState& paste = gAppState.paste;
    if (paste.canvas == kCanvasInvalid) {
        return;
    }
    ZoneScopedN("PasteUpdate");
    CanvasState& canvas = gAppState.canvases[paste.canvas];
    const CanvasView view = WindowCanvasView(paste.window);
    if (canvas.id != paste.canvas || view == kCanvasViewInvalid || gAppState.canvasViews[view].canvas != paste.canvas
        || HasAnyFlag(canvas.layers[paste.layer].flags, LayerFlags::Deleted)) {
        paste = {};
        return;
    }
    auto& slots = gAppState.transfers.uploads;
    const bool wasModified = canvas.modified;
    while (!paste.remaining.empty()) {
        const auto free = std::ranges::find(slots, TileSlotState::Free, &TileSlot::state);
        if (free == slots.end()) {
            break;
        }
        const auto slot = static_cast<TileTransferSlot>(free - slots.begin());
        const TileGridPos gridPos = paste.remaining.back();
        paste.remaining.pop_back();
        if (!PasteTileFill(paste, gridPos, GraphicsTileUploadMemory(slot))) {
            continue;
        }
        const TileKey key = TileKeyMake(paste.layer, 0, gridPos);
        TileInsert(canvas, key, kTileAtlasCellInvalid).transfer = TileTransfer::Load;
        LayerCompositesTouch(canvas, key);
        *free = TileSlot{
            .fence = 0, .key = key, .cell = kTileAtlasCellInvalid, .canvas = canvas.id, .state = TileSlotState::Ready};
        canvas.modified = true;
    }
    CanvasEdited(canvas.id, wasModified);
    const bool loading = std::ranges::any_of(canvas.tiles, [&](const TileState& tile) {
        return TileKeyLayer(tile.key) == paste.layer && HasAnyFlag(tile.transfer, TileTransfer::Load);
    });
    if (!paste.remaining.empty() || loading || gAppState.stroke.canvas != kCanvasInvalid
        || canvas.history.pending != 0) {
        return;
    }
    if (!paste.selected) {
        const Layer selection = LayerSelectionTake(canvas);
        if (selection == kLayerInvalid) {
            paste = {};
            return;
        }
        const auto x = static_cast<double>(paste.x);
        const auto y = static_cast<double>(paste.y);
        const double right = x + paste.width;
        const double bottom = y + paste.height;
        StrokeBegin(StrokeBeginInfo{
            .brush = {},
            .color = Color::Srgb{.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f},
            .point = {.x = x, .y = y, .pressure = 1.0f},
            .layer = selection,
            .view = view,
            .mode = StrokeMode::Replace,
            .blendMode = BlendMode::Normal,
            .shape = StrokeShape::Rect,
        });
        StrokeTo(StrokePoint{.x = right, .y = bottom, .pressure = 1.0f});
        StrokeEnd();
        canvas.selectionBounds = SelectionBounds{.minX = x, .minY = y, .maxX = right, .maxY = bottom};
        paste.selected = true;
        return;
    }
    const Window window = paste.window;
    const bool active = canvas.activeLayer == paste.layer;
    paste = {};
    if (active) {
        if (const auto res = AppTransformMode(window); !res) {
            MIDORI_LOG_WARN("paste: no transform: {}", ErrorMessage(res.error()));
        }
    }
}

// selection.clear: a Replace stroke with no dab clears every tile of the mask, as one undo step.
void SelectionClear(Window window) noexcept {
    const CanvasView view = WindowCanvasView(window);
    if (view == kCanvasViewInvalid || gAppState.stroke.canvas != kCanvasInvalid) {
        return;
    }
    CanvasState& canvas = gAppState.canvases[gAppState.canvasViews[view].canvas];
    if (canvas.selection == kLayerInvalid || SelectionEmpty(canvas.selectionBounds) || canvas.history.pending != 0
        || canvas.merge.result != kLayerInvalid) {
        return;
    }
    const bool wasModified = canvas.modified;
    StrokeBegin(StrokeBeginInfo{
        .brush = {},
        .color = Color::Srgb{.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f},
        .point = {},
        .layer = canvas.selection,
        .view = view,
        .mode = StrokeMode::Replace,
        .blendMode = BlendMode::Normal,
        .shape = StrokeShape::Rect,
    });
    canvas.selectionBounds = {};
    StrokeEnd();
    CanvasEdited(canvas.id, wasModified);
}

// selection.delete: an Erase stroke of one rectangle over the selection's bounds, which the selection clips, as one
// undo step. A texel larger each side, so its antialiased edge leaves the selection's own.
void SelectionDelete(Window window) noexcept {
    if (ActionBlocked(window, ActionId::SelectionDelete, 0) != Text::Count_) {
        return;
    }
    const CanvasView view = WindowCanvasView(window);
    CanvasState& canvas = gAppState.canvases[gAppState.canvasViews[view].canvas];
    if (canvas.history.pending != 0) {
        return;
    }
    const SelectionBounds& bounds = canvas.selectionBounds;
    const bool wasModified = canvas.modified;
    StrokeBegin(StrokeBeginInfo{
        .brush = {},
        .color = Color::Srgb{.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f},
        .point = {.x = bounds.minX - 1.0, .y = bounds.minY - 1.0, .pressure = 1.0f},
        .layer = canvas.activeLayer,
        .view = view,
        .mode = StrokeMode::Erase,
        .blendMode = BlendMode::Normal,
        .shape = StrokeShape::Rect,
    });
    StrokeTo(StrokePoint{.x = bounds.maxX + 1.0, .y = bounds.maxY + 1.0, .pressure = 1.0f});
    StrokeEnd();
    CanvasEdited(canvas.id, wasModified);
}

// edit.copy and edit.cut: the active layer's pixels times the selection's coverage, over the selection's bounds, to the
// clipboard. Read a row of tiles at a time, so memory stays bounded whatever the selection. Cut then deletes them.
void AppCopy(Window window, bool cut) noexcept {
    ZoneScopedN("AppCopy");
    if (ActionBlocked(window, cut ? ActionId::EditCut : ActionId::EditCopy, 0) != Text::Count_) {
        return;
    }
    const CanvasState& canvas = *WindowCanvas(window);
    const SelectionBounds& bounds = canvas.selectionBounds;
    const auto minX = static_cast<std::int64_t>(std::floor(bounds.minX));
    const auto minY = static_cast<std::int64_t>(std::floor(bounds.minY));
    const auto maxX = static_cast<std::int64_t>(std::ceil(bounds.maxX));
    const auto maxY = static_cast<std::int64_t>(std::ceil(bounds.maxY));
    if (maxX - minX > kPlatformImageSideMax || maxY - minY > kPlatformImageSideMax) {
        MIDORI_LOG_WARN("copy: {}x{} is too large", maxX - minX, maxY - minY);
        return;
    }
    PlatformImage image{
        .pixels = std::vector<std::byte>(static_cast<std::size_t>((maxX - minX) * (maxY - minY) * 4)),
        .width = static_cast<std::uint32_t>(maxX - minX),
        .height = static_cast<std::uint32_t>(maxY - minY),
    };
    const auto tileOf = [](std::int64_t pos) {
        return static_cast<TileAxis>(pos >= 0 ? pos / kTileSize : ((pos + 1) / kTileSize) - 1);
    };
    constexpr std::int64_t kSize = kTileSize;
    const TileAxis firstX = tileOf(minX);
    const auto columns = static_cast<std::size_t>(tileOf(maxX - 1) - firstX + 1);
    const std::size_t tileBytes = TileBytes(canvas.pixelFormat);
    // Per column, the layer's tile then the selection's.
    std::vector<TileKey> keys(columns * 2);
    std::vector<std::byte> pixels(keys.size() * tileBytes);
    for (TileAxis tileRow = tileOf(minY); tileRow <= tileOf(maxY - 1); ++tileRow) {
        for (std::size_t c = 0; c < columns; ++c) {
            const TileGridPos gridPos{.x = static_cast<TileAxis>(firstX + static_cast<TileAxis>(c)), .y = tileRow};
            keys[c * 2] = TileKeyMake(canvas.activeLayer, 0, gridPos);
            keys[(c * 2) + 1] = TileKeyMake(canvas.selection, 0, gridPos);
        }
        if (const auto res = CanvasTilesGet(canvas, keys, pixels); !res) {
            MIDORI_LOG_ERROR("copy: {}", ErrorMessage(res.error()));
            return;
        }
        const std::int64_t tileY = std::int64_t{tileRow} * kSize;
        for (std::size_t c = 0; c < columns; ++c) {
            const std::byte* layer = pixels.data() + (c * 2 * tileBytes);
            const std::byte* mask = layer + tileBytes;
            const std::int64_t tileX = (std::int64_t{firstX} + static_cast<std::int64_t>(c)) * kSize;
            for (std::int64_t y = std::max(minY, tileY); y < std::min(maxY, tileY + kSize); ++y) {
                for (std::int64_t x = std::max(minX, tileX); x < std::min(maxX, tileX + kSize); ++x) {
                    const auto texel = static_cast<std::size_t>((((y - tileY) * kSize) + (x - tileX)) * 4);
                    const unsigned coverage = std::to_integer<unsigned>(mask[texel + 3]);
                    std::byte* out = image.pixels.data() + ((((y - minY) * image.width) + (x - minX)) * 4);
                    for (std::size_t channel = 0; channel < 4; ++channel) {
                        out[channel] = static_cast<std::byte>(
                            ((std::to_integer<unsigned>(layer[texel + channel]) * coverage) + 127) / 255);
                    }
                }
            }
        }
    }
    if (const auto res = PlatformClipboardSetImage(window, image); !res) {
        MIDORI_LOG_ERROR("copy: {}", ErrorMessage(res.error()));
        return;
    }
    gAppState.copy = CopyState{
        .x = minX,
        .y = minY,
        .width = image.width,
        .height = image.height,
        .sequence = PlatformClipboardSequence(),
        .copied = true,
    };
    if (cut) {
        SelectionDelete(window);
    }
}

// The drag's tiles to its canvas's checked tags, then no drag.
void TagDragEnd() noexcept {
    TagDragState& drag = gAppState.tagDrag;
    CanvasState& canvas = gAppState.canvases[gAppState.canvasViews[drag.view].canvas];
    if (const std::uint64_t checked = canvas.tagsChecked & canvas.tags; checked != 0) {
        const bool wasModified = canvas.modified;
        TagsApply(canvas, TagShapeTiles(drag.shape, drag.points), drag.subtract, checked);
        canvas.modified = true;
        CanvasEdited(canvas.id, wasModified);
    }
    drag = {};
}

// Left drag with a Tags tool. Like a stroke, releases and focus loss end it even when the UI wants them. Returns true
// when the event belongs to the drag.
[[nodiscard]] bool TagDrag(const Event& event, bool uiWants) noexcept {
    ZoneScopedN("TagDrag");
    // Finer than a tile is no use; the cap bounds a lasso's cost.
    constexpr double kSpacing = kTileSize / 16.0;
    constexpr std::size_t kPointsMax = 4096;
    TagDragState& drag = gAppState.tagDrag;
    const bool dragging = drag.view != kCanvasViewInvalid;
    switch (event.type) {
        case EventType::Button: {
            if (event.button.button != Button::Left) {
                return false;
            }
            if (event.button.inputState == InputState::Release) {
                if (dragging) {
                    TagDragEnd();
                }
                return dragging;
            }
            const ToolState& tool = ToolCurrent(gAppState.tools);
            const CanvasView view = WindowCanvasView(event.button.window);
            if (event.button.inputState != InputState::Press || uiWants || view == kCanvasViewInvalid || dragging
                || tool.target != ToolTarget::Tags || gAppState.stroke.canvas != kCanvasInvalid) {
                return false;
            }
            const auto point = CanvasPointUnderPointer(view, 1.0f);
            if (!point) {
                return false;
            }
            // Only adding and subtracting mean something for tags: every other operation adds.
            drag = TagDragState{
                .points = {TagPoint{.x = point->x, .y = point->y}},
                .view = view,
                .shape = tool.shape,
                .subtract = SelectionOpHeld() == SelectionOp::Subtract,
            };
            return true;
        }
        case EventType::Pointer: {
            if (!dragging || gAppState.canvasViews[drag.view].window != event.pointer.window) {
                return false;
            }
            if (const auto point = CanvasPointUnderPointer(drag.view, 1.0f)) {
                const TagPoint to{.x = point->x, .y = point->y};
                if (drag.shape == ToolShape::Box) {
                    drag.points.resize(1);
                    drag.points.push_back(to);
                } else if (drag.points.size() < kPointsMax
                           && std::hypot(to.x - drag.points.back().x, to.y - drag.points.back().y) >= kSpacing) {
                    drag.points.push_back(to);
                }
            }
            return true;
        }
        case EventType::WindowFocus:
            if (dragging && !event.windowFocus.focused) {
                TagDragEnd();
            }
            return false;
        default:
            return false;
    }
}

// The texel at the canvas point (x, y) of the level-`level` tile at `gridPos`, laid out in `pixels` as TileLevelBytes,
// premultiplied, from 0 to 1.
[[nodiscard]] std::array<float, 4> TileTexelAt(std::span<const std::byte> pixels, PixelFormat format, TileMip level,
                                               TileGridPos gridPos, double x, double y) noexcept {
    MIDORI_ASSERT(format == PixelFormat::RGBA8, "eyedropper reads RGBA8 only");
    const auto texel = [level](double pos, TileAxis origin) {
        const double local = std::clamp(std::floor(pos - (static_cast<double>(origin) * kTileSize)), 0.0,
                                        static_cast<double>(kTileSize - 1));
        return std::size_t{static_cast<std::uint32_t>(local) >> level};
    };
    const std::size_t offset =
        (texel(y, gridPos.y) * TileLevelPitch(level, format)) + (texel(x, gridPos.x) * PixelFormatTexelBytes(format));
    std::array<float, 4> value{};
    for (std::size_t c = 0; c < value.size(); ++c) {
        value[c] = static_cast<float>(std::to_integer<std::uint8_t>(pixels[offset + c])) / 255.0f;
    }
    return value;
}

// The color `view`'s canvas has at the canvas point, by `source`. Canvas: the root folder's tile there, at the level
// the view draws it, over the canvas's background; nothing while that tile is not loaded. Layer: the active layer's
// level-0 texel, wherever it is stored. Alpha 0 where there is none. Waits for the GPU, or the read.
[[nodiscard]] std::optional<Color::Srgb> EyedropperColorAt(CanvasView view, double x, double y,
                                                           EyedropperSource source) noexcept {
    ZoneScopedN("EyedropperColorAt");
    const CanvasViewState& viewState = gAppState.canvasViews[view];
    const CanvasState& canvas = gAppState.canvases[viewState.canvas];
    const TileGridPos gridPos{.x = static_cast<TileAxis>(std::floor(x / kTileSize)),
                              .y = static_cast<TileAxis>(std::floor(y / kTileSize))};
    std::array<float, 4> shown{};
    std::vector<std::byte> pixels(TileBytes(canvas.pixelFormat));
    if (source == EyedropperSource::Layer) {
        if (canvas.activeLayer == kLayerInvalid) {
            return Color::Srgb{};
        }
        const TileKey key = TileKeyMake(canvas.activeLayer, 0, gridPos);
        if (const auto res = CanvasTilesGet(canvas, std::span(&key, 1), pixels); !res) {
            MIDORI_LOG_ERROR("eyedropper: CanvasTilesGet failed: {}", ErrorMessage(res.error()));
            return std::nullopt;
        }
        shown = TileTexelAt(pixels, canvas.pixelFormat, 0, gridPos, x, y);
    } else if (const TileKey key = TileKeyMake(canvas.layersOrder.back(), 0, gridPos);
               canvas.tileIndices.contains(key)) {
        const TileMip level = TileDrawnLevel(canvas, key, TileLevelForZoom(viewState.viewport.zoomX));
        if (level == kTileMipMax) {
            return std::nullopt;
        }
        const auto found = canvas.tileIndices.find(TileKeyWithMip(key, level));
        MIDORI_ASSERT(found != canvas.tileIndices.end(), "drawn level {} of {:#018x} missing", level, key);
        if (const auto res = GraphicsTilesDownload(std::span(&canvas.tiles[found->second].cell, 1), pixels); !res) {
            MIDORI_LOG_ERROR("eyedropper: GraphicsTilesDownload failed: {}", ErrorMessage(res.error()));
            return std::nullopt;
        }
        shown = TileTexelAt(pixels, canvas.pixelFormat, level, gridPos, x, y);
    }
    // Premultiplied, like the renderer lays the canvas over its background.
    const Color::Srgb& background = canvas.backgroundColor;
    const float under = source == EyedropperSource::Canvas ? background.a * (1.0f - shown[3]) : 0.0f;
    const float alpha = shown[3] + under;
    if (alpha <= 0.0f) {
        return Color::Srgb{};
    }
    const auto channel = [alpha, under](float premultiplied, float backgroundChannel) {
        return std::clamp((premultiplied + (backgroundChannel * under)) / alpha, 0.0f, 1.0f);
    };
    return Color::Srgb{
        .r = channel(shown[0], background.r),
        .g = channel(shown[1], background.g),
        .b = channel(shown[2], background.b),
        .a = alpha,
    };
}

// The current color slot takes the color shown under the drag's last point, keeping its own alpha. Where nothing is
// shown it stays; while the tile there streams in, the next update tries again.
void EyedropperSample() noexcept {
    ZoneScopedN("EyedropperSample");
    EyedropperState& eyedropper = gAppState.eyedropper;
    const auto color = EyedropperColorAt(eyedropper.view, eyedropper.x, eyedropper.y, gAppState.tools.eyedropperSource);
    if (!color) {
        return;
    }
    eyedropper.moved = false;
    if (color->a <= 0.0f) {
        return;
    }
    ToolsState& tools = gAppState.tools;
    Color::Srgb& slot = tools.colors[tools.colorSlot];
    slot = Color::Srgb{.r = color->r, .g = color->g, .b = color->b, .a = slot.a};
}

// Left drag picks the color under the pointer while AppEyedropperActive. Like a stroke, releases and focus loss end it
// even when the UI wants them; letting go of tool.eyedropper does not. Returns true when the event belongs to the drag.
[[nodiscard]] bool Eyedropper(const Event& event, bool uiWants) noexcept {
    ZoneScopedN("Eyedropper");
    EyedropperState& eyedropper = gAppState.eyedropper;
    const bool picking = eyedropper.view != kCanvasViewInvalid;
    const auto follow = [] {
        EyedropperState& drag = gAppState.eyedropper;
        if (const auto point = CanvasPointUnderPointer(drag.view, 1.0f)) {
            drag.x = point->x;
            drag.y = point->y;
            drag.moved = true;
        }
    };
    switch (event.type) {
        case EventType::Button: {
            if (event.button.button != Button::Left) {
                return false;
            }
            if (event.button.inputState == InputState::Release) {
                if (picking) {
                    if (eyedropper.moved) {
                        EyedropperSample();
                    }
                    eyedropper = {};
                }
                return picking;
            }
            const CanvasView view = WindowCanvasView(event.button.window);
            if (event.button.inputState != InputState::Press || uiWants || view == kCanvasViewInvalid || picking
                || gAppState.stroke.canvas != kCanvasInvalid || !AppEyedropperActive()) {
                return false;
            }
            eyedropper.view = view;
            follow();
            return true;
        }
        case EventType::Pointer:
            if (!picking || gAppState.canvasViews[eyedropper.view].window != event.pointer.window) {
                return false;
            }
            follow();
            return true;
        case EventType::WindowFocus:
            if (picking && !event.windowFocus.focused) {
                eyedropper = {};
            }
            return false;
        default:
            return false;
    }
}

// Left drag paints or erases the active layer, by the current tool. Releases and focus loss end the stroke even when
// the UI wants them. Returns true when the event belongs to the stroke.
[[nodiscard]] bool Paint(const Event& event, bool uiWants) noexcept {
    ZoneScopedN("Paint");
    StrokeState& stroke = gAppState.stroke;
    const bool painting = stroke.canvas != kCanvasInvalid && !stroke.ended && stroke.shape != StrokeShape::Transform;
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
            const ToolState& tool = ToolCurrent(gAppState.tools);
            if (tool.target == ToolTarget::Tags || AppEyedropperActive()) {
                return false;
            }
            const bool selecting = tool.target == ToolTarget::Selection;
            if (const CanvasState& target = gAppState.canvases[gAppState.canvasViews[view].canvas];
                target.history.pending != 0 || target.merge.result != kLayerInvalid
                || (!selecting && (!GraphicsBrushLoaded(tool.shader) || !LayerPaintable(target, target.activeLayer)))) {
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
            const std::optional<StrokeBeginInfo> beginInfo =
                selecting ? SelectionBeginInfo(view, *point,
                                               tool.shape == ToolShape::Lasso ? StrokeShape::Lasso : StrokeShape::Rect)
                          : StrokeBeginInfoFromTool(view, *point);
            if (!beginInfo) {
                return false;
            }
            StrokeBegin(*beginInfo);
            if (!selecting && tool.blendMode != BlendMode::Erase) {
                ColorRecentPush(gAppState.tools, beginInfo->color);
            }
            SelectionBoundsFollow();
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
                SelectionBoundsFollow();
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

[[nodiscard]] Result<void> ActionRun(Window window, ActionId action, std::uint32_t arg) noexcept {
    ZoneScopedN("ActionRun");
    AppState& app = gAppState;
    std::erase(app.recentActions, action);
    app.recentActions.insert(app.recentActions.begin(), action);
    if (app.recentActions.size() > kRecentActionsMax) {
        app.recentActions.pop_back();
    }
    switch (action) {
        case ActionId::AppQuit:
            return AppQuit();
        case ActionId::AppSearch:
            if (window < kWindowMax) {
                gUiState.searchShown[window] = true;
                gUiState.searchQuery[window] = {};
                gUiState.searchSelected[window] = 0;
                gUiState.searchFocus[window] = true;
            }
            return {};
        case ActionId::CanvasNew:
            return AppCanvasNew(window);
        case ActionId::CanvasOpen:
            return AppCanvasOpen(window);
        case ActionId::CanvasSave:
            return AppCanvasSave(window);
        case ActionId::CanvasSaveAs:
            return AppCanvasSaveAs(window);
        case ActionId::HistoryUndo:
            // Undoing a transform in progress cancels it.
            if (AppTransforming()) {
                AppTransformEnd(false);
                return {};
            }
            return AppUndo(window);
        case ActionId::HistoryRedo:
            return AppRedo(window);
        case ActionId::WindowFullscreen:
            return AppToggleFullscreen(window);
        case ActionId::UiToggle:
            if (window < kWindowMax) {
                gUiState.hidden[window] = !gUiState.hidden[window];
            }
            return {};
        case ActionId::BrushRadius:
            BrushAdjustBegin(window, BrushAdjustMode::Radius);
            return {};
        case ActionId::BrushOpacity:
            BrushAdjustBegin(window, BrushAdjustMode::Opacity);
            return {};
        case ActionId::BrushHardness:
            BrushAdjustBegin(window, BrushAdjustMode::Hardness);
            return {};
        case ActionId::LayerNew:
            AppLayerNew(window, false);
            return {};
        case ActionId::LayerNewFolder:
            AppLayerNew(window, true);
            return {};
        case ActionId::LayerDelete:
            AppLayerDelete(window);
            return {};
        case ActionId::LayerMerge:
            AppLayerMerge(window);
            return {};
        case ActionId::LayerSelectAbove:
        case ActionId::LayerSelectBelow:
            AppLayerSelect(window, action == ActionId::LayerSelectAbove);
            return {};
        case ActionId::LayerMoveUp:
        case ActionId::LayerMoveDown:
            AppLayerMove(window, action == ActionId::LayerMoveUp);
            return {};
        case ActionId::LayerToggleHidden:
            AppLayerToggleHidden(window);
            return {};
        case ActionId::LayerBlendNext:
        case ActionId::LayerBlendPrevious:
            AppLayerBlendModeCycle(window, action == ActionId::LayerBlendNext);
            return {};
        case ActionId::LayerOpacity:
            BrushAdjustBegin(window, BrushAdjustMode::LayerOpacity);
            return {};
        case ActionId::ViewNavigate:
            app.spaceDown = true;
            return {};
        case ActionId::ViewReset:
            ViewReset(window);
            return {};
        case ActionId::ViewFlipHorizontal:
        case ActionId::ViewFlipVertical:
            ViewFlip(window, action == ActionId::ViewFlipVertical);
            return {};
        case ActionId::ViewBack:
        case ActionId::ViewForward:
            ViewStep(window, action == ActionId::ViewForward);
            return {};
        case ActionId::LayerActivate:
            if (CanvasState* canvas = WindowCanvas(window);
                canvas && arg < kLayerMax && canvas->layerHeights[arg] != kLayerInvalid
                && !HasAnyFlag(canvas->layers[arg].flags, LayerFlags::Internal)) {
                canvas->activeLayer = static_cast<Layer>(arg);
                canvas->selectedLayers.assign(1, canvas->activeLayer);
            }
            return {};
        case ActionId::CanvasOpenRecent:
            return AppCanvasOpenRecent(window, arg);
        case ActionId::CanvasRecentClear:
            app.recentFiles.clear();
            return {};
        case ActionId::PanelSettings:
        case ActionId::PanelView:
        case ActionId::PanelTool:
        case ActionId::PanelToolbar:
        case ActionId::PanelTools:
        case ActionId::PanelLayers:
        case ActionId::PanelTags:
        case ActionId::PanelColor:
        case ActionId::PanelHelp:
        case ActionId::PanelMenuBar:
        case ActionId::DebugTileAtlas:
        case ActionId::DebugTileCells:
        case ActionId::DebugStatistics:
        case ActionId::DebugImGuiMetrics:
            if (window < kWindowMax) {
                bool& shown = action == ActionId::PanelSettings     ? gUiState.settingsShown[window]
                            : action == ActionId::PanelView         ? gUiState.viewShown[window]
                            : action == ActionId::PanelTool         ? gUiState.toolShown[window]
                            : action == ActionId::PanelToolbar      ? gUiState.toolbarShown[window]
                            : action == ActionId::PanelTools        ? gUiState.toolsShown[window]
                            : action == ActionId::PanelLayers       ? gUiState.layersShown[window]
                            : action == ActionId::PanelTags         ? gUiState.tagsShown[window]
                            : action == ActionId::PanelColor        ? gUiState.colorShown[window]
                            : action == ActionId::PanelHelp         ? gUiState.helpShown[window]
                            : action == ActionId::PanelMenuBar      ? gUiState.menuBarShown[window]
                            : action == ActionId::DebugTileAtlas    ? gUiState.tileAtlasShown[window]
                            : action == ActionId::DebugStatistics   ? gUiState.statisticsShown[window]
                            : action == ActionId::DebugImGuiMetrics ? gUiState.imguiMetricsShown[window]
                                                                    : gUiState.tileCellsShown[window];
                shown = !shown;
            }
            return {};
        case ActionId::SettingsLanguage:
            if (arg == 0) {
                app.language.clear();
                TextLoad(PlatformLanguage());
            } else if (arg <= kLanguages.size()) {
                app.language = kLanguages[arg - 1].code;
                TextLoad(app.language);
            }
            return {};
        case ActionId::SettingsCanvasFilter:
            if (arg < std::to_underlying(CanvasFilter::Count_)) {
                app.canvasFilter = static_cast<CanvasFilter>(arg);
            }
            return {};
        case ActionId::ToolSelect:
            ToolPick(app.tools, arg);
            return {};
        case ActionId::ToolbarSlot:
            ToolbarSlotPress(app.tools, arg);
            return {};
        case ActionId::ToolDuplicate:
            ToolDuplicate(app.tools, arg);
            return {};
        case ActionId::ToolDelete:
            ToolDelete(app.tools, arg);
            return {};
        case ActionId::SelectionOp:
            if (arg < std::to_underlying(SelectionOp::Count_)) {
                ToolCurrent(app.tools).selectionOp = static_cast<SelectionOp>(arg);
            }
            return {};
        case ActionId::SelectionTransform:
            // One begun with Ctrl held then lasts until applied.
            if (AppTransforming()) {
                app.transform.held = false;
                return {};
            }
            return AppTransformMode(window);
        case ActionId::TransformApply:
        case ActionId::TransformCancel:
            AppTransformEnd(action == ActionId::TransformApply);
            return {};
        case ActionId::TagSelect:
        case ActionId::TagToggle:
            if (CanvasState* canvas = WindowCanvas(window); canvas && arg < kTagMax) {
                const std::uint64_t bit = std::uint64_t{1} << arg;
                canvas->tagsSelected = action == ActionId::TagSelect ? bit : canvas->tagsSelected ^ bit;
                TagSelectedIslandsBuild(*canvas);
                TagIslandShow(window, 0);
            }
            return {};
        case ActionId::TagNext:
        case ActionId::TagPrevious:
            TagIslandShow(window, action == ActionId::TagNext ? 1 : -1);
            return {};
        case ActionId::SelectionClear:
            SelectionClear(window);
            return {};
        case ActionId::EditPaste:
            AppPaste(window);
            return {};
        case ActionId::EditCopy:
        case ActionId::EditCut:
            AppCopy(window, action == ActionId::EditCut);
            return {};
        case ActionId::SelectionDelete:
            SelectionDelete(window);
            return {};
        case ActionId::BrushOpen:
            return AppBrushOpen(window);
        case ActionId::BrushBuiltIn:
            BrushShaderSet({});
            return {};
        case ActionId::BrushReload:
            BrushShaderReload(ToolCurrent(app.tools).shader);
            return {};
        case ActionId::ColorNext:
        case ActionId::ColorPrevious: {
            const auto count = static_cast<std::uint32_t>(app.tools.colors.size());
            app.tools.colorSlot = (app.tools.colorSlot + (action == ActionId::ColorNext ? 1U : count - 1U)) % count;
            return {};
        }
        case ActionId::ToolEyedropper:
            // Read through KeymapHeld while its keys are down, see AppEyedropperActive.
            return {};
        case ActionId::TagDelete:
            AppTagDelete(window, arg);
            return {};
        case ActionId::BrushTexture:
            return arg < kMdslTexturesMax ? AppBrushTextureOpen(window, arg) : Result<void>{};
        case ActionId::BrushExample:
            if (arg < gAppState.brushExamples.size()) {
                BrushShaderSet(gAppState.brushExamples[arg]);
            }
            return {};
        case ActionId::Count_:
            break;
    }
    return {};
}

// A Hold action lets go once the last key of its binding is up.
void ActionEnd(ActionId action) noexcept {
    if (action == ActionId::ViewNavigate) {
        gAppState.spaceDown = false;
        NavigateEnd();
    }
}

// Releases reach the keymap even when the UI wants the keyboard, so a Hold action never sticks.
[[nodiscard]] Result<void> KeyDispatch(const KeyEvent& key, bool uiWants) noexcept {
    ZoneScopedN("KeyDispatch");
    AppState& app = gAppState;
    KeymapState& keymap = app.keymap;
    const auto lastKey = [&](std::int32_t binding) {
        const KeyBinding& b = keymap.bindings[static_cast<std::size_t>(binding)];
        return b.chords[b.length - 1].key;
    };
    if (key.key < Key::Count_) {
        keymap.down[std::to_underlying(key.key)] = key.inputState != InputState::Release;
    }
    if (key.inputState == InputState::Release) {
        if (keymap.holding >= 0 && lastKey(keymap.holding) == key.key) {
            ActionEnd(keymap.bindings[static_cast<std::size_t>(keymap.holding)].action);
            keymap.holding = -1;
        }
        if (keymap.repeating >= 0 && lastKey(keymap.repeating) == key.key) {
            keymap.repeating = -1;
        }
        return {};
    }
    if (uiWants || key.key >= Key::LeftShift) {
        return {};
    }
    if (key.inputState == InputState::Hold) {
        if (keymap.repeating < 0 || lastKey(keymap.repeating) != key.key) {
            return {};
        }
        const KeyBinding& binding = keymap.bindings[static_cast<std::size_t>(keymap.repeating)];
        return ActionRun(key.window, binding.action, binding.arg);
    }

    keymap.repeating = -1;
    ActionContext context = ActionContext::Global;
    if (app.spaceDown) {
        context = ActionContext::Navigate;
    } else if (WindowCanvasView(key.window) != kCanvasViewInvalid) {
        context = ActionContext::Canvas;
    }
    const std::int32_t index = KeymapPress(keymap, KeyChord{.key = key.key, .mods = key.keyMods}, context);
    if (index < 0) {
        return {};
    }
    const KeyBinding& binding = keymap.bindings[static_cast<std::size_t>(index)];
    switch (binding.trigger) {
        case ActionKind::Hold:
            // Read through KeymapHeld while its keys are down.
            if (kActions[std::to_underlying(binding.action)].kind != ActionKind::Hold) {
                return {};
            }
            keymap.holding = index;
            break;
        case ActionKind::Repeat:
            keymap.repeating = index;
            break;
        case ActionKind::Press:
            break;
    }
    // A key pressed with Ctrl still held after a transform it began applies it first, unless it undoes it.
    if (AppTransforming() && app.transform.held && app.transform.drag == TransformDrag::None
        && binding.action != ActionId::HistoryUndo && binding.action != ActionId::TransformCancel
        && binding.action != ActionId::SelectionTransform && binding.action != ActionId::ViewNavigate) {
        AppTransformEnd(true);
    }
    return ActionRun(key.window, binding.action, binding.arg);
}

// What an update leaves for the next ones, which present until it is done: a stroke the GPU has not recorded, undo
// steps and transfers in flight, a merge, the GPU work the next submit records, or streaming that ran out of budget.
[[nodiscard]] bool AppBusy(bool streaming) noexcept {
    const AppState& app = gAppState;
    const StrokeState& stroke = app.stroke;
    if (streaming || app.paste.canvas != kCanvasInvalid || !app.historyPool.ops.empty()
        || std::ranges::any_of(app.tools.shaders,
                               [](const ToolShader& shader) { return shader.compiling || shader.ready; })
        || (stroke.canvas != kCanvasInvalid
            && (stroke.ended || std::ranges::any_of(stroke.tiles, StrokeTilePending)))) {
        return true;
    }
    const auto inFlight = [](const TileSlot& slot) {
        return slot.state != TileSlotState::Free;
    };
    if (std::ranges::any_of(app.transfers.uploads, inFlight)
        || std::ranges::any_of(app.transfers.downloads, inFlight)) {
        return true;
    }
    return std::ranges::any_of(app.canvases, [](const CanvasState& canvas) {
        return canvas.id != kCanvasInvalid
            && (canvas.history.pending != 0 || canvas.merge.result != kLayerInvalid || !canvas.creates.empty()
                || !canvas.mipBuilds.empty() || !canvas.composites.empty());
    });
}

} // namespace

Result<void> AppInit(AppInitInfo initInfo) noexcept {
    ZoneScopedN("AppInit");
    MIDORI_LOG_INFO("{} v{} {}-{} {}", MIDORI_NAME, MIDORI_VERSION_STRING, MIDORI_COMPILER, MIDORI_ARCHITECTURE,
                    MIDORI_PROFILE);

    // The built-in shader, whose params tools.json reads those of tools without a file of their own by.
    const auto builtIn = MdslTranslate(round_mdsl);
    MIDORI_ASSERT_ALWAYS(builtIn.has_value(), "round.mdsl: {}", builtIn ? "" : builtIn.error().message);
    gAppState.tools = ToolsLoad(*builtIn);

    // Before any window opens: new windows show the panels the preferences keep.
    if (const auto preferences = PreferencesLoad()) {
        gAppState.tools.colors = preferences->colors;
        gAppState.tools.colorSlot = preferences->colorSlot;
        gAppState.tools.recentColors = preferences->recentColors;
        gUiState.preferences = preferences->ui;
        gAppState.windowPlacement = preferences->window;
        gAppState.canvasFilter = preferences->canvasFilter;
        gAppState.zoomMin = preferences->zoomMin;
        gAppState.zoomMax = preferences->zoomMax;
        gAppState.recentFiles = preferences->recentFiles;
        gAppState.openFiles = preferences->openFiles;
        gAppState.reopenFiles = preferences->reopenFiles;
        gAppState.language = preferences->language;
    } else {
        MIDORI_LOG_WARN("preferences not loaded, defaults used: {}", ErrorMessage(preferences.error()));
    }
    TextLoad(gAppState.language.empty() ? PlatformLanguage() : gAppState.language);
    KeymapLoad(gAppState.keymap);
    UiFontsLoad();

    if (const auto folder = PlatformExecutableFolder()) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(*folder / "brushes", ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() == ".mdsl") {
                gAppState.brushExamples.push_back(it->path());
            }
        }
        std::ranges::sort(gAppState.brushExamples);
    }

    // GraphicsInit created atlas 0. The clear page is held by no tile.
    gAppState.atlas.count = 1;
    gAppState.atlas.used[kTileAtlasPageClear] = ~std::uint64_t{0};
    // Largest first, so pop_back gives the smallest.
    gAppState.historyPool.freeSlots.reserve(kHistorySlotsMax);
    for (std::uint32_t slot = kHistorySlotsMax; slot > 0; --slot) {
        gAppState.historyPool.freeSlots.push_back(slot - 1);
    }
    JobsInit(JobsInitInfo{.execute = CanvasJobExecute, .readerCount = 0});

    // Gone files are skipped quietly: only the user's own choice of file is worth an error.
    std::vector<std::filesystem::path> reopened;
    std::vector<std::uint64_t> reopenedTagsChecked;
    if (initInfo.files.empty() && gAppState.reopenFiles) {
        std::error_code ec;
        for (const OpenFile& file : gAppState.openFiles) {
            if (std::filesystem::is_regular_file(file.path, ec)) {
                reopened.push_back(file.path);
                reopenedTagsChecked.push_back(file.tagsChecked);
            }
        }
        initInfo.files = reopened;
    }
    gAppState.openFiles.clear();

    // One window per file, or one new canvas. A file that fails to open leaves a new canvas in its window.
    // The platform owns the windows and destroys them in PlatformTerminate.
    const std::size_t windowCount = std::max<std::size_t>(initInfo.files.size(), 1);
    for (std::size_t i = 0; i < windowCount; ++i) {
        // The first where the last one was left, the others where the system puts them.
        WindowCreateInfo createInfo = i == 0 ? gAppState.windowPlacement : WindowCreateInfo{};
        createInfo.title = MIDORI_NAME;
        const auto window = WindowCreate(createInfo);
        if (!window) {
            MIDORI_LOG_ERROR("WindowCreate failed: {}", ErrorMessage(window.error()));
            // Files already shown keep the app running; without any window it cannot start.
            return i == 0 ? std::unexpected(window.error()) : Result<void>{};
        }
        if (i < initInfo.files.size() && WindowOpenFile(*window, initInfo.files[i])) {
            if (CanvasState* canvas = WindowCanvas(*window); canvas && i < reopenedTagsChecked.size()) {
                canvas->tagsChecked = reopenedTagsChecked[i] & canvas->tags;
            }
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
        gAppState.pointerTime = event.pointer.time;
        gAppState.pointerX = event.pointer.posX;
        gAppState.pointerY = event.pointer.posY;
        gAppState.pointerPressure = event.pointer.pressure;
        gAppState.pointerTiltX = event.pointer.tiltX;
        gAppState.pointerTiltY = event.pointer.tiltY;
    }
    const bool uiWants = UiEvent(event);
    if (BrushAdjust(event) || Navigate(event, uiWants) || TransformEvent(event, uiWants) || TagDrag(event, uiWants)
        || Eyedropper(event, uiWants) || Paint(event, uiWants)) {
        return {};
    }
    if (event.type == EventType::Key) {
        MIDORI_LOG_TRACE("window {}: key {} state {} mods {:#04x}", event.key.window, std::to_underlying(event.key.key),
                         std::to_underlying(event.key.inputState), std::to_underlying(event.key.keyMods));
        return KeyDispatch(event.key, uiWants);
    }
    if (uiWants) {
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
            // Closing the last window is an exit: only the files left open then are reopened.
            if (WindowOpenCount() == 1) {
                gAppState.openFiles.clear();
            }
            return AppWindowClose(event.windowClose.window);
        case EventType::WindowDestroy:
            if (const TagDragState& drag = gAppState.tagDrag;
                drag.view != kCanvasViewInvalid
                && gAppState.canvasViews[drag.view].window == event.windowDestroy.window) {
                gAppState.tagDrag = {};
            }
            // The window owns its views.
            for (CanvasViewState& view : gAppState.canvasViews) {
                if (view.id != kCanvasViewInvalid && view.window == event.windowDestroy.window) {
                    CanvasViewDetach(view.id);
                }
            }
            break;
        case EventType::Key:
            break;
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
    UiWindowFrame(window, viewport, backgroundColor, gAppState.tools);
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
    if (UiWantsFrame(window)) {
        GraphicsWindowRequestFrame(window);
    }
}

Result<void> AppUpdate() noexcept {
    ZoneScopedN("AppUpdate");
    // A limit changed in the settings may leave a view past it.
    for (CanvasViewState& view : gAppState.canvasViews) {
        if (view.id != kCanvasViewInvalid) {
            view.viewport = ViewportZoom(view.viewport, 1.0f, 1.0f, gAppState.zoomMin, gAppState.zoomMax);
        }
    }
    TilesMarkVisible();
    StrokeUpdate();
    BrushShadersUpdate();
    PasteUpdate();
    for (const CanvasState& canvas : gAppState.canvases) {
        if (canvas.id != kCanvasInvalid) {
            const bool wasModified = canvas.modified;
            HistoryUpdate(canvas.id);
            CanvasEdited(canvas.id, wasModified);
        }
    }
    const bool streaming = CanvasTilesStream();
    if (gAppState.eyedropper.view != kCanvasViewInvalid && gAppState.eyedropper.moved) {
        EyedropperSample();
    }
    // A dialog an action opens may close windows, so each request checks its own.
    for (const ActionRequest& request : std::exchange(gAppState.actionRequests, {})) {
        if (!WindowGetState(request.window)) {
            continue;
        }
        if (const auto res = ActionRun(request.window, request.action, request.arg); !res) {
            return res;
        }
    }
    // Otherwise a window presents only after input, see UiWantsFrame, and sleeps in between.
    gAppState.busy = AppBusy(streaming);
    if (gAppState.busy) {
        for (const CanvasViewState& view : gAppState.canvasViews) {
            if (view.id != kCanvasViewInvalid) {
                GraphicsWindowRequestFrame(view.window);
            }
        }
    }
    return {};
}

// The windows still open are destroyed after this, by PlatformTerminate.
void AppTerminate() noexcept {
    ZoneScopedN("AppTerminate");
    // A brush being adjusted is kept as it is.
    for (Window window = 0; window < kWindowMax; ++window) {
        if (WindowGetState(window)) {
            WindowPlacementKeep(window);
            break;
        }
    }
    const Preferences preferences{
        .ui = gUiState.preferences,
        .window = gAppState.windowPlacement,
        .canvasFilter = gAppState.canvasFilter,
        .zoomMin = gAppState.zoomMin,
        .zoomMax = gAppState.zoomMax,
        .recentFiles = gAppState.recentFiles,
        .openFiles = gAppState.openFiles,
        .language = gAppState.language,
        .colors = gAppState.tools.colors,
        .recentColors = gAppState.tools.recentColors,
        .colorSlot = gAppState.tools.colorSlot,
        .reopenFiles = gAppState.reopenFiles,
    };
    if (const auto res = PreferencesSave(preferences); !res) {
        MIDORI_LOG_ERROR("PreferencesSave failed: {}", ErrorMessage(res.error()));
    }
    if (const auto res = ToolsSave(gAppState.tools); !res) {
        MIDORI_LOG_ERROR("ToolsSave failed: {}", ErrorMessage(res.error()));
    }
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
        AppError(window, TextGet(Text::ErrorCreate), canvas.error());
        return std::unexpected(canvas.error());
    }
    WindowShowCanvas(window, *canvas);
    return {};
}

Result<void> AppCanvasOpenRecent(Window window, std::size_t index) noexcept {
    ZoneScopedN("AppCanvasOpenRecent");
    if (index >= gAppState.recentFiles.size()) {
        return {};
    }
    // Copied: opening reorders the list.
    const std::filesystem::path path = gAppState.recentFiles[index];
    if (const CanvasView view = WindowCanvasView(window);
        view != kCanvasViewInvalid && !CanvasViewReleaseConfirmed(view)) {
        return {};
    }
    const auto opened = WindowOpenFile(window, path);
    if (!opened) {
        std::erase(gAppState.recentFiles, path);
    }
    return opened;
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

void AppTagNew(Window window) noexcept {
    CanvasState* canvas = WindowCanvas(window);
    if (!canvas || canvas->tags == ~std::uint64_t{0}) {
        return;
    }
    const auto tag = static_cast<std::uint32_t>(std::countr_one(canvas->tags));
    const bool wasModified = canvas->modified;
    canvas->tags |= std::uint64_t{1} << tag;
    canvas->tagsChecked |= std::uint64_t{1} << tag;
    canvas->tagHues[tag] = TagHueDefault(tag);
    const std::string name = TextFormat(Text::TagName, static_cast<int>(tag + 1));
    canvas->tagNames[tag] = {};
    std::copy_n(name.begin(), std::min(name.size(), kTagNameLenMax - 1), canvas->tagNames[tag].begin());
    canvas->modified = true;
    CanvasEdited(canvas->id, wasModified);
}

void AppTagRename(Window window, std::uint32_t tag, const char* name) noexcept {
    CanvasState* canvas = WindowCanvas(window);
    const std::string_view text(name);
    // The file knows a tag by its name, so it is never empty.
    if (!canvas || tag >= kTagMax || (canvas->tags & (std::uint64_t{1} << tag)) == 0 || text.empty()
        || text == canvas->tagNames[tag].data()) {
        return;
    }
    const bool wasModified = canvas->modified;
    canvas->tagNames[tag] = {};
    std::copy_n(text.begin(), std::min(text.size(), kTagNameLenMax - 1), canvas->tagNames[tag].begin());
    canvas->modified = true;
    CanvasEdited(canvas->id, wasModified);
}

void AppTagHue(Window window, std::uint32_t tag, float hue) noexcept {
    CanvasState* canvas = WindowCanvas(window);
    if (!canvas || tag >= kTagMax || (canvas->tags & (std::uint64_t{1} << tag)) == 0 || hue == canvas->tagHues[tag]) {
        return;
    }
    const bool wasModified = canvas->modified;
    canvas->tagHues[tag] = std::clamp(hue, 0.0f, 1.0f);
    canvas->modified = true;
    CanvasEdited(canvas->id, wasModified);
}

void AppTagDelete(Window window, std::uint32_t tag) noexcept {
    const auto exists = [tag](const CanvasState* canvas) {
        return canvas && tag < kTagMax && (canvas->tags & (std::uint64_t{1} << tag)) != 0;
    };
    const CanvasState* asked = WindowCanvas(window);
    if (!exists(asked)) {
        return;
    }
    const std::string instruction = TextFormat(Text::AskTagDelete, asked->tagNames[tag].data());
    gAppState.modal = true;
    const DialogChoice choice = DialogAsk(DialogAskInfo{
        .title = MIDORI_NAME,
        .instruction = instruction,
        .content = TextGet(Text::AskTagDeleteContent),
        .yes = TextGet(Text::TagDelete),
        .no = {},
        .window = window,
    });
    gAppState.modal = false;
    // The dialog's message loop may have closed the window meanwhile.
    CanvasState* canvas = WindowCanvas(window);
    if (choice != DialogChoice::Yes || canvas != asked || !exists(canvas)) {
        return;
    }
    const std::uint64_t bit = std::uint64_t{1} << tag;
    const bool wasModified = canvas->modified;
    for (TagTile& tile : canvas->tagTiles) {
        tile.tags &= ~bit;
    }
    std::erase_if(canvas->tagTiles, [](const TagTile& tile) { return tile.tags == 0; });
    canvas->tags &= ~bit;
    canvas->tagsChecked &= ~bit;
    canvas->tagsSelected &= ~bit;
    canvas->tagNames[tag] = {};
    TagIslandsBuild(*canvas);
    for (int& renaming : gUiState.tagRenaming) {
        renaming = renaming == static_cast<int>(tag) ? -1 : renaming;
    }
    canvas->modified = true;
    CanvasEdited(canvas->id, wasModified);
}

Text ActionBlocked(Window window, ActionId action, std::uint32_t arg) noexcept {
    const AppState& app = gAppState;
    const CanvasView view = WindowCanvasView(window);
    const CanvasState* canvas = view == kCanvasViewInvalid ? nullptr : &app.canvases[app.canvasViews[view].canvas];
    const bool painting = app.stroke.canvas != kCanvasInvalid && !app.stroke.ended;
    const auto needsCanvas = [&] {
        return canvas ? Text::Count_ : Text::ReasonNoCanvas;
    };
    const auto needsEdit = [&] {
        if (!canvas) {
            return Text::ReasonNoCanvas;
        }
        return canvas->merge.result != kLayerInvalid ? Text::ReasonMerging : Text::Count_;
    };
    const auto needsLayer = [&] {
        const Text blocked = needsEdit();
        if (blocked != Text::Count_) {
            return blocked;
        }
        return canvas->activeLayer == kLayerInvalid ? Text::ReasonNoLayer : Text::Count_;
    };
    switch (action) {
        case ActionId::CanvasSave:
        case ActionId::CanvasSaveAs:
        case ActionId::LayerNew:
        case ActionId::LayerNewFolder:
            return needsEdit();
        case ActionId::HistoryUndo:
        case ActionId::HistoryRedo: {
            if (!canvas) {
                return Text::ReasonNoCanvas;
            }
            if (app.paste.canvas != kCanvasInvalid) {
                return Text::ReasonPainting;
            }
            if (AppTransforming()) {
                return action == ActionId::HistoryUndo ? Text::Count_ : Text::ReasonPainting;
            }
            const std::int32_t at = static_cast<std::int32_t>(canvas->history.current) + canvas->history.pending;
            if (action == ActionId::HistoryUndo) {
                return at > 0 ? Text::Count_ : Text::ReasonNoUndo;
            }
            return at < static_cast<std::int32_t>(canvas->history.records.size()) ? Text::Count_ : Text::ReasonNoRedo;
        }
        case ActionId::ToolSelect:
        case ActionId::ToolDuplicate:
            return ToolFind(app.tools, arg) ? Text::Count_ : Text::ReasonNoTool;
        case ActionId::ToolDelete:
            if (!ToolFind(app.tools, arg)) {
                return Text::ReasonNoTool;
            }
            return app.tools.library.size() > 1 ? Text::Count_ : Text::ReasonLastTool;
        case ActionId::ToolbarSlot:
            return std::ranges::contains(app.tools.slots, arg, &ToolbarSlot::id) ? Text::Count_ : Text::ReasonNoSlot;
        case ActionId::BrushRadius:
        case ActionId::BrushOpacity:
        case ActionId::BrushHardness:
            if (!canvas) {
                return Text::ReasonNoCanvas;
            }
            return painting ? Text::ReasonPainting : Text::Count_;
        case ActionId::LayerOpacity:
            if (painting) {
                return Text::ReasonPainting;
            }
            return needsLayer();
        case ActionId::LayerDelete:
        case ActionId::LayerMerge:
        case ActionId::LayerMoveUp:
        case ActionId::LayerMoveDown:
        case ActionId::LayerToggleHidden:
        case ActionId::LayerBlendNext:
        case ActionId::LayerBlendPrevious:
            return needsLayer();
        case ActionId::LayerSelectAbove:
        case ActionId::LayerSelectBelow:
            if (!canvas) {
                return Text::ReasonNoCanvas;
            }
            // The root folder is always last in the order.
            return canvas->layersOrder.size() > 2 ? Text::Count_ : Text::ReasonOneLayer;
        case ActionId::LayerActivate:
        case ActionId::ViewReset:
        case ActionId::ViewFlipHorizontal:
        case ActionId::ViewFlipVertical:
            return needsCanvas();
        case ActionId::ViewBack:
        case ActionId::ViewForward: {
            if (!canvas) {
                return Text::ReasonNoCanvas;
            }
            const CanvasViewState& state = app.canvasViews[view];
            if (action == ActionId::ViewBack) {
                return state.viewportUndo.empty() ? Text::ReasonNoViewBack : Text::Count_;
            }
            return state.viewportRedo.empty() ? Text::ReasonNoViewForward : Text::Count_;
        }
        case ActionId::ViewNavigate:
            return Text::ReasonHold;
        case ActionId::SelectionClear:
            if (!canvas) {
                return Text::ReasonNoCanvas;
            }
            if (canvas->selection == kLayerInvalid || SelectionEmpty(canvas->selectionBounds)) {
                return Text::ReasonNoSelection;
            }
            return painting ? Text::ReasonPainting : needsEdit();
        case ActionId::EditCopy:
        case ActionId::EditCut: {
            const Text blocked = needsLayer();
            if (blocked != Text::Count_) {
                return blocked;
            }
            if (canvas->selection == kLayerInvalid || SelectionEmpty(canvas->selectionBounds)) {
                return Text::ReasonNoSelection;
            }
            if (painting || app.paste.canvas != kCanvasInvalid) {
                return Text::ReasonPainting;
            }
            // Copying reads any layer that has pixels; cutting also erases them.
            const bool readable = !HasAnyFlag(canvas->layers[canvas->activeLayer].flags, LayerFlags::Folder);
            const bool paintable = LayerPaintable(*canvas, canvas->activeLayer);
            return (action == ActionId::EditCut ? paintable : readable) ? Text::Count_ : Text::ReasonNotPaintable;
        }
        case ActionId::EditPaste: {
            const Text blocked = needsEdit();
            if (blocked != Text::Count_) {
                return blocked;
            }
            if (painting || app.paste.canvas != kCanvasInvalid) {
                return Text::ReasonPainting;
            }
            return PlatformClipboardHasImage() ? Text::Count_ : Text::ReasonNoImage;
        }
        case ActionId::SelectionDelete: {
            const Text blocked = needsLayer();
            if (blocked != Text::Count_) {
                return blocked;
            }
            if (canvas->selection == kLayerInvalid || SelectionEmpty(canvas->selectionBounds)) {
                return Text::ReasonNoSelection;
            }
            if (app.stroke.canvas != kCanvasInvalid) {
                return Text::ReasonPainting;
            }
            return LayerPaintable(*canvas, canvas->activeLayer) ? Text::Count_ : Text::ReasonNotPaintable;
        }
        case ActionId::SelectionTransform: {
            const Text blocked = needsLayer();
            if (blocked != Text::Count_) {
                return blocked;
            }
            if (canvas->selection == kLayerInvalid || SelectionEmpty(canvas->selectionBounds)) {
                return Text::ReasonNoSelection;
            }
            if (app.stroke.canvas != kCanvasInvalid) {
                return Text::ReasonPainting;
            }
            if (!LayerPaintable(*canvas, canvas->activeLayer)) {
                return Text::ReasonNotPaintable;
            }
            return TransformTileCount(canvas->selectionBounds) > kStrokeTransformTilesMax
                     ? Text::ReasonSelectionTooLarge
                     : Text::Count_;
        }
        case ActionId::TransformApply:
        case ActionId::TransformCancel:
            return AppTransforming() ? Text::Count_ : Text::ReasonNoTransform;
        case ActionId::CanvasOpenRecent:
            return arg < app.recentFiles.size() ? Text::Count_ : Text::ReasonNoRecent;
        case ActionId::CanvasRecentClear:
            return app.recentFiles.empty() ? Text::ReasonNoRecent : Text::Count_;
        case ActionId::BrushBuiltIn:
            return app.stroke.canvas != kCanvasInvalid ? Text::ReasonPainting : Text::Count_;
        case ActionId::BrushReload:
            return BrushShaderStroking(ToolCurrent(app.tools).shader) ? Text::ReasonPainting : Text::Count_;
        case ActionId::BrushTexture:
            if (arg >= ToolSource(app.tools, ToolCurrent(app.tools)).textures.size()) {
                return Text::ReasonNoTexture;
            }
            return app.stroke.canvas != kCanvasInvalid ? Text::ReasonPainting : Text::Count_;
        case ActionId::BrushExample:
            return arg < app.brushExamples.size() ? Text::Count_ : Text::ReasonNoExample;
        case ActionId::TagSelect:
        case ActionId::TagToggle:
        case ActionId::TagDelete:
            if (!canvas) {
                return Text::ReasonNoCanvas;
            }
            return arg < kTagMax && (canvas->tags & (std::uint64_t{1} << arg)) != 0 ? Text::Count_ : Text::ReasonNoTag;
        case ActionId::TagNext:
        case ActionId::TagPrevious:
            if (!canvas) {
                return Text::ReasonNoCanvas;
            }
            if (canvas->tagsSelected == 0) {
                return Text::ReasonNoTagSelected;
            }
            return canvas->tagSelectedIslands.empty() ? Text::ReasonTagEmpty : Text::Count_;
        default:
            return Text::Count_;
    }
}

float AppZoomSupported(Window window) noexcept {
    const auto state = WindowGetState(window);
    return state ? TileZoomMin(static_cast<float>(state->width) / state->dpiScale,
                               static_cast<float>(state->height) / state->dpiScale)
                 : kViewportZoomMin;
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

Result<void> AppTransformMode(Window window) noexcept {
    ZoneScopedN("AppTransformMode");
    if (ActionBlocked(window, ActionId::SelectionTransform, 0) != Text::Count_) {
        return {};
    }
    CanvasState& canvas = *WindowCanvas(window);
    if (canvas.history.pending != 0) {
        return {};
    }
    const SelectionBounds& source = canvas.selectionBounds;
    gAppState.transform = TransformState{.wasModified = canvas.modified};
    StrokeBegin(StrokeBeginInfo{
        .brush = {},
        .color = Color::Srgb{.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f},
        .point = {.x = (source.minX + source.maxX) * 0.5, .y = (source.minY + source.maxY) * 0.5, .pressure = 1.0f},
        .layer = canvas.activeLayer,
        .view = WindowCanvasView(window),
        .mode = StrokeMode::Paint,
        .blendMode = BlendMode::Normal,
        .shape = StrokeShape::Transform,
    });
    CanvasEdited(canvas.id, gAppState.transform.wasModified);
    return {};
}

// Applying what moves nothing, or canceling, puts every tile back from its backup and records nothing.
void AppTransformEnd(bool apply) noexcept {
    ZoneScopedN("AppTransformEnd");
    if (!AppTransforming()) {
        return;
    }
    StrokeState& stroke = gAppState.stroke;
    CanvasState& canvas = gAppState.canvases[stroke.canvas];
    gAppState.transform.drag = TransformDrag::None;
    if (!apply || stroke.transform == StrokeTransform{}) {
        StrokeTransformTo(StrokeTransform{});
        canvas.selectionBounds = stroke.selectionBefore;
        stroke.discard = true;
        if (!gAppState.transform.wasModified && canvas.modified) {
            canvas.modified = false;
            CanvasTitlesUpdate(canvas.id);
        }
    }
    StrokeEnd();
}

bool AppEyedropperActive() noexcept {
    const ToolState& tool = ToolCurrent(gAppState.tools);
    if (tool.target == ToolTarget::Color) {
        return true;
    }
    return tool.target == ToolTarget::Layer && tool.shape == ToolShape::Path && tool.blendMode != BlendMode::Erase
        && KeymapHeld(gAppState.keymap, ActionId::ToolEyedropper, ActionContext::Canvas, gAppState.keyMods) >= 0;
}

bool AppTransforming() noexcept {
    const StrokeState& stroke = gAppState.stroke;
    return stroke.canvas != kCanvasInvalid && !stroke.ended && stroke.shape == StrokeShape::Transform;
}

std::optional<TransformBox> AppTransformBox(Window window) noexcept {
    const AppState& app = gAppState;
    const CanvasView view = WindowCanvasView(window);
    if (view == kCanvasViewInvalid) {
        return std::nullopt;
    }
    if (AppTransforming()) {
        if (app.stroke.view != view) {
            return std::nullopt;
        }
        return TransformBox{.transform = app.stroke.transform, .source = app.stroke.selectionBefore, .view = view};
    }
    if (!HasAnyFlag(app.keyMods, KeyMod::Ctrl) || app.spaceDown || app.navigate.mode != NavigateMode::None
        || ActionBlocked(window, ActionId::SelectionTransform, 0) != Text::Count_) {
        return std::nullopt;
    }
    return TransformBox{
        .transform = {},
        .source = app.canvases[app.canvasViews[view].canvas].selectionBounds,
        .view = view,
    };
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

Result<void> AppQuit() noexcept {
    ZoneScopedN("AppQuit");
    gAppState.openFiles.clear();
    for (Window window = 0; window < kWindowMax; ++window) {
        if (!WindowGetState(window)) {
            continue;
        }
        if (const auto res = AppWindowClose(window); !res) {
            return res;
        }
        if (WindowGetState(window)) {
            break;
        }
    }
    return {};
}

Result<void> AppWindowClose(Window window) noexcept {
    ZoneScopedN("AppWindowClose");
    if (const CanvasView view = WindowCanvasView(window);
        view != kCanvasViewInvalid && !CanvasViewReleaseConfirmed(view)) {
        return {};
    }
    // Closing the last window ends the app, with no window left to ask at exit.
    WindowPlacementKeep(window);
    if (const CanvasState* canvas = WindowCanvas(window);
        canvas && !canvas->path.empty() && !std::ranges::contains(gAppState.openFiles, canvas->path, &OpenFile::path)
        && gAppState.openFiles.size() < kWindowMax) {
        gAppState.openFiles.push_back(OpenFile{.path = canvas->path, .tagsChecked = canvas->tagsChecked});
    }
    return WindowDestroy(window);
}

} // namespace Midori
