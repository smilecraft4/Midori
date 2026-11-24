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
#include <iterator>
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
// Wheel notches that double or halve the zoom.
constexpr float kWheelZoomNotches = 4.0f;
// Wheel notches and zoom keys this close after the last one join its navigation history step.
constexpr std::chrono::milliseconds kViewStepMerge{600};
// brush.larger and brush.smaller multiply or divide the size by it.
constexpr float kBrushSizeStep = 1.25f;
// How often, at most, a modified canvas is written to the recovery folder.
constexpr std::chrono::minutes kAutosaveInterval{3};

constexpr std::string_view kCanvasTypeName = "Midori canvas";
constexpr std::string_view kCanvasExtension = "mori";

static_assert(kCanvasViewMax >= kWindowMax, "a window without a view always finds a free one");

[[nodiscard]] CanvasView WindowCanvasView(Window window) noexcept {
    return AppWindowView(window);
}

[[nodiscard]] bool CanvasShownElsewhere(Canvas canvas, CanvasView view) noexcept {
    return std::ranges::any_of(gAppState.canvasViews, [canvas, view](const CanvasViewState& other) {
        return other.id != kCanvasViewInvalid && other.id != view && other.canvas == canvas;
    });
}

[[nodiscard]] bool CanvasShownOutside(Canvas canvas, Window window) noexcept {
    return std::ranges::any_of(gAppState.canvasViews, [canvas, window](const CanvasViewState& other) {
        return other.id != kCanvasViewInvalid && other.window != window && other.canvas == canvas;
    });
}

// The focused view's canvas, or the app's name alone.
void WindowTitleUpdate(Window window) noexcept {
    if (!WindowGetState(window)) {
        return;
    }
    const CanvasView view = WindowCanvasView(window);
    const std::string title =
        view == kCanvasViewInvalid
            ? std::string(MIDORI_NAME)
            : std::format("{} - {}", AppCanvasTitle(gAppState.canvasViews[view].canvas), MIDORI_NAME);
    if (const auto res = WindowSetTitle(window, title); !res) {
        MIDORI_LOG_ERROR("WindowSetTitle failed: {}", ErrorMessage(res.error()));
    }
}

// The last node takes the place of `node`, which nothing names any more.
void LayoutNodeRemove(Window window, std::uint8_t node) noexcept {
    WindowLayout& layout = gAppState.layouts[window];
    const auto last = static_cast<std::uint8_t>(layout.count - 1);
    if (node != last) {
        layout.nodes[node] = layout.nodes[last];
        for (std::uint8_t i = 0; i < last; ++i) {
            LayoutNode& other = layout.nodes[i];
            if (other.split != LayoutSplit::None) {
                other.first = other.first == last ? node : other.first;
                other.second = other.second == last ? node : other.second;
            }
        }
        for (CanvasViewState& view : gAppState.canvasViews) {
            if (view.id != kCanvasViewInvalid && view.window == window && view.pane == last) {
                view.pane = node;
            }
        }
        layout.focused = layout.focused == last ? node : layout.focused;
    }
    --layout.count;
}

// A pane with no tab gives its split's place to its sibling. The root pane stays.
void LayoutPaneCollapse(Window window, std::uint8_t pane) noexcept {
    WindowLayout& layout = gAppState.layouts[window];
    if (pane == 0) {
        return;
    }
    std::uint8_t parent = 0;
    while (layout.nodes[parent].split == LayoutSplit::None
           || (layout.nodes[parent].first != pane && layout.nodes[parent].second != pane)) {
        ++parent;
        MIDORI_ASSERT(parent < layout.count, "pane {} of window {} has no parent", pane, window);
    }
    const std::uint8_t sibling =
        layout.nodes[parent].first == pane ? layout.nodes[parent].second : layout.nodes[parent].first;
    layout.nodes[parent] = layout.nodes[sibling];
    for (CanvasViewState& view : gAppState.canvasViews) {
        if (view.id != kCanvasViewInvalid && view.window == window && view.pane == sibling) {
            view.pane = parent;
        }
    }
    if (layout.focused == pane || layout.focused == sibling) {
        layout.focused = parent;
    }
    // While a split's two nodes remain, `focused` names a leaf: the parent took the sibling's place.
    while (layout.nodes[layout.focused].split != LayoutSplit::None) {
        layout.focused = layout.nodes[layout.focused].first;
    }
    LayoutNodeRemove(window, std::max(pane, sibling));
    LayoutNodeRemove(window, std::min(pane, sibling));
}

// The view's pane shows it and takes the focus.
void ViewActivate(CanvasView view) noexcept {
    const CanvasViewState& state = gAppState.canvasViews[view];
    WindowLayout& layout = gAppState.layouts[state.window];
    layout.nodes[state.pane].active = view;
    layout.focused = state.pane;
    WindowTitleUpdate(state.window);
}

// A new tab at the end of the pane, shown and focused. kCanvasViewInvalid when every view is taken.
[[nodiscard]] CanvasView ViewTabAdd(Window window, std::uint8_t pane, Canvas canvas,
                                    const Viewport& viewport) noexcept {
    AppState& app = gAppState;
    const auto free = std::ranges::find(app.canvasViews, kCanvasViewInvalid, &CanvasViewState::id);
    if (free == app.canvasViews.end()) {
        MIDORI_LOG_ERROR("all {} canvas views are in use", kCanvasViewMax);
        return kCanvasViewInvalid;
    }
    const auto view = static_cast<CanvasView>(free - app.canvasViews.begin());
    std::array<CanvasView, kCanvasViewMax> tabs{};
    *free = CanvasViewState{
        .viewport = viewport,
        .rect = {},
        .id = view,
        .canvas = canvas,
        .window = window,
        .pane = pane,
        .tab = static_cast<std::uint8_t>(AppPaneTabs(window, pane, tabs)),
    };
    ViewActivate(view);
    return view;
}

// Out of its pane: the tabs after it move left, the pane shows the one now in its place or the last, and a pane left
// with none goes.
void ViewTabRemove(CanvasView view) noexcept {
    const CanvasViewState& state = gAppState.canvasViews[view];
    const Window window = state.window;
    const std::uint8_t pane = state.pane;
    std::array<CanvasView, kCanvasViewMax> tabs{};
    const std::uint32_t count = AppPaneTabs(window, pane, tabs);
    CanvasView shown = kCanvasViewInvalid;
    for (std::uint32_t i = state.tab + 1; i < count; ++i) {
        --gAppState.canvasViews[tabs[i]].tab;
    }
    if (count > 1) {
        shown = tabs[state.tab + 1u < count ? state.tab + 1u : state.tab - 1u];
    }
    WindowLayout& layout = gAppState.layouts[window];
    if (layout.nodes[pane].active == view) {
        layout.nodes[pane].active = shown;
    }
    gAppState.canvasViews[view].window = kWindowInvalid;
    if (count == 1) {
        LayoutPaneCollapse(window, pane);
    }
}

// The node and what it holds over `area`: a split's two sides around its handle, a pane's active view under its tab
// strip, which shows with two tabs or more.
void LayoutLay(Window window, float dpiScale, std::uint8_t index, CanvasViewRect area) noexcept {
    LayoutNode& node = gAppState.layouts[window].nodes[index];
    node.rect = area;
    if (node.split == LayoutSplit::None) {
        if (node.active == kCanvasViewInvalid) {
            return;
        }
        std::array<CanvasView, kCanvasViewMax> tabs{};
        const int strip = AppPaneTabs(window, index, tabs) > 1
                            ? std::min(static_cast<int>(std::lround(kLayoutTabStripDips * dpiScale)), area.height)
                            : 0;
        gAppState.canvasViews[node.active].rect =
            CanvasViewRect{.x = area.x, .y = area.y + strip, .width = area.width, .height = area.height - strip};
        return;
    }
    const int gap = kLayoutSplitterPixels;
    const bool columns = node.split == LayoutSplit::Columns;
    const int room = std::max((columns ? area.width : area.height) - gap, 0);
    const int first = std::clamp(static_cast<int>(std::lround(static_cast<float>(room) * node.share)), 0, room);
    CanvasViewRect a = area;
    CanvasViewRect b = area;
    if (columns) {
        a.width = first;
        b.x = area.x + first + gap;
        b.width = room - first;
    } else {
        a.height = first;
        b.y = area.y + first + gap;
        b.height = room - first;
    }
    const std::uint8_t firstNode = node.first;
    const std::uint8_t secondNode = node.second;
    LayoutLay(window, dpiScale, firstNode, a);
    LayoutLay(window, dpiScale, secondNode, b);
}

// The pane holding the client-area point, the focused one when none does.
[[nodiscard]] std::uint8_t LayoutPaneAt(Window window, float x, float y) noexcept {
    const WindowLayout& layout = gAppState.layouts[window];
    for (std::uint8_t i = 0; i < layout.count; ++i) {
        const LayoutNode& node = layout.nodes[i];
        if (node.split == LayoutSplit::None && x >= static_cast<float>(node.rect.x)
            && x < static_cast<float>(node.rect.x + node.rect.width) && y >= static_cast<float>(node.rect.y)
            && y < static_cast<float>(node.rect.y + node.rect.height)) {
            return i;
        }
    }
    return layout.focused;
}

// The shown view under the pointer in the window it last moved in, else the window's focused one.
[[nodiscard]] CanvasView WindowViewUnderPointer(Window window) noexcept {
    const CanvasView view =
        gAppState.layouts[window].nodes[LayoutPaneAt(window, gAppState.pointerX, gAppState.pointerY)].active;
    return view == kCanvasViewInvalid ? WindowCanvasView(window) : view;
}

// A canvas nobody can see is gone: its unsaved changes were saved or discarded before, see
// CanvasViewReleaseConfirmed.
void CanvasViewDetach(CanvasView view) noexcept {
    ZoneScopedN("CanvasViewDetach");
    CanvasViewState& viewState = gAppState.canvasViews[view];
    const Canvas canvas = viewState.canvas;
    const Window window = viewState.window;
    ViewTabRemove(view);
    viewState = {};
    WindowTitleUpdate(window);
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

// The windows whose focused view shows the canvas.
void CanvasTitlesUpdate(Canvas canvas) noexcept {
    ZoneScopedN("CanvasTitlesUpdate");
    for (Window window = 0; window < kWindowMax; ++window) {
        if (const CanvasView view = WindowCanvasView(window);
            view != kCanvasViewInvalid && gAppState.canvasViews[view].canvas == canvas) {
            WindowTitleUpdate(window);
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

// The window shows `canvas`: its view there when it has one, else a new tab of the focused pane at a fresh viewport.
// Error::CanvasViewLimitReached when every view is taken; a canvas no view shows then is destroyed.
[[nodiscard]] Result<void> WindowShowCanvas(Window window, Canvas canvas) noexcept {
    ZoneScopedN("WindowShowCanvas");
    for (const CanvasViewState& view : gAppState.canvasViews) {
        if (view.id != kCanvasViewInvalid && view.window == window && view.canvas == canvas) {
            ViewActivate(view.id);
            return {};
        }
    }
    const float zoom = std::clamp(gAppState.zoomDefault, gAppState.zoomMin, gAppState.zoomMax);
    if (ViewTabAdd(window, gAppState.layouts[window].focused, canvas, Viewport{.zoomX = zoom, .zoomY = zoom})
        == kCanvasViewInvalid) {
        if (!CanvasShownElsewhere(canvas, kCanvasViewInvalid)) {
            CanvasDestroy(canvas);
        }
        return std::unexpected(Error::CanvasViewLimitReached);
    }
    return {};
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
    if (!saveAs && state.file.env && state.recovered.empty()) {
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

// The file in a tab of the window, see WindowShowCanvas. Failures are shown to the user.
[[nodiscard]] Result<void> WindowOpenFile(Window window, const std::filesystem::path& path) noexcept {
    ZoneScopedN("WindowOpenFile");
    // Already open, and so locked: show that canvas.
    std::error_code ec;
    Canvas opened = kCanvasInvalid;
    for (const CanvasState& canvas : gAppState.canvases) {
        if (canvas.id != kCanvasInvalid && !canvas.path.empty() && std::filesystem::equivalent(canvas.path, path, ec)) {
            opened = canvas.id;
            break;
        }
    }
    if (opened == kCanvasInvalid) {
        const auto canvas = CanvasOpen(path);
        if (!canvas) {
            AppError(window, TextFormat(Text::ErrorOpen, PathUtf8(path.filename()).c_str()), canvas.error());
            return std::unexpected(canvas.error());
        }
        opened = *canvas;
    }
    if (const auto shown = WindowShowCanvas(window, opened); !shown) {
        AppError(window, TextFormat(Text::ErrorOpen, PathUtf8(path.filename()).c_str()), shown.error());
        return shown;
    }
    RecentFilesAdd(path);
    return {};
}

struct ViewPoint {
    float x;
    float y;
};

// Client-area physical pixels to view DIPs around the view center, floored to a whole pixel like the
// renderer's projection.
[[nodiscard]] ViewPoint ViewPointFromPointer(const CanvasViewRect& rect, float dpiScale, float pointerX,
                                             float pointerY) noexcept {
    const float centerX = static_cast<float>(rect.x) + std::floor(static_cast<float>(rect.width) * 0.5f);
    const float centerY = static_cast<float>(rect.y) + std::floor(static_cast<float>(rect.height) * 0.5f);
    return ViewPoint{.x = (pointerX - centerX) / dpiScale, .y = (pointerY - centerY) / dpiScale};
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
    const ViewPoint point =
        ViewPointFromPointer(viewState.rect, windowState->dpiScale, gAppState.pointerX, gAppState.pointerY);
    // Moving the view by minus the point's offset brings the canvas point under it to the center.
    const Viewport under = ViewportMove(viewState.viewport, -point.x, -point.y);
    return StrokePoint{.x = under.posX,
                       .y = under.posY,
                       .time = gAppState.pointerTime,
                       .pressure = pressure,
                       .tiltX = gAppState.pointerTiltX,
                       .tiltY = gAppState.pointerTiltY};
}

// Starts a frame: the windows' layouts are laid out, the root folder tiles any shown view overlaps get the new frame as
// `visibleFrame`, and the grid positions there where a layer has a tile become the canvas's composite wants. Layer
// tiles are wanted by the composites that need them. A rotated view marks the tiles of its bounding box.
void TilesMarkVisible() noexcept {
    ZoneScopedN("TilesMarkVisible");
    const std::uint32_t frame = ++gAppState.frame;
    for (Window window = 0; window < kWindowMax; ++window) {
        AppLayoutUpdate(window);
    }
    for (CanvasState& canvas : gAppState.canvases) {
        canvas.compositeWants.clear();
    }
    for (const CanvasViewState& view : gAppState.canvasViews) {
        if (view.id == kCanvasViewInvalid || view.rect.width <= 0 || view.rect.height <= 0) {
            continue;
        }
        const auto windowState = WindowGetState(view.window);
        if (!windowState) {
            continue;
        }
        const auto left = static_cast<float>(view.rect.x);
        const auto top = static_cast<float>(view.rect.y);
        const auto right = static_cast<float>(view.rect.x + view.rect.width);
        const auto bottom = static_cast<float>(view.rect.y + view.rect.height);
        double minX = std::numeric_limits<double>::max();
        double minY = std::numeric_limits<double>::max();
        double maxX = std::numeric_limits<double>::lowest();
        double maxY = std::numeric_limits<double>::lowest();
        for (const auto [cornerX, cornerY] :
             {std::pair{left, top}, std::pair{right, top}, std::pair{left, bottom}, std::pair{right, bottom}}) {
            const ViewPoint point = ViewPointFromPointer(view.rect, windowState->dpiScale, cornerX, cornerY);
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
        const TileMip level = TileLevelForZoom(view.viewport.zoomX, gAppState.tileLodBias);
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
    gAppState.viewStepView = kCanvasViewInvalid;
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
    gAppState.viewStepView = kCanvasViewInvalid;
}

// Before a wheel notch or zoom key changes the view: a history step of its own, unless a drag holds one already or the
// last notch or key on this view was moments ago, so a run of them is one step.
void ViewStepBegin(CanvasView view) noexcept {
    AppState& app = gAppState;
    const auto now = std::chrono::steady_clock::now();
    if (app.navigate.mode == NavigateMode::None
        && (app.viewStepView != view || now - app.viewStepTime > kViewStepMerge)) {
        NavigationPush(view, app.canvasViews[view].viewport);
    }
    app.viewStepView = view;
    app.viewStepTime = now;
}

[[nodiscard]] bool ViewportEqual(const Viewport& a, const Viewport& b) noexcept {
    return a.posX == b.posX && a.posY == b.posY && a.zoomX == b.zoomX && a.zoomY == b.zoomY && a.rot == b.rot;
}

// Starts a drag with `button`, or switches the one in progress to `mode` from where it is, keeping its button.
void NavigateBegin(CanvasView view, NavigateMode mode, Button button = Button::Left) noexcept {
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
        .button = dragging ? gAppState.navigate.button : button,
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
    const auto [startX, startY] =
        ViewPointFromPointer(view.rect, windowState->dpiScale, navigate.pointerStartX, navigate.pointerStartY);
    const auto [x, y] = ViewPointFromPointer(view.rect, windowState->dpiScale, gAppState.pointerX, gAppState.pointerY);
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
    const float zoom = std::clamp(app.zoomDefault, app.zoomMin, app.zoomMax);
    app.canvasViews[view].viewport =
        ViewportSnap(ViewportResetZoomRotation(app.canvasViews[view].viewport, zoom), dpiScale);
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
    const CanvasViewRect& rect = app.canvasViews[view].rect;
    const double fit = 0.9
                     * std::min(static_cast<double>(rect.width) / ((width * c) + (height * s)),
                                static_cast<double>(rect.height) / ((width * s) + (height * c)));
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

// The next zoom past `zoom`, in or out, among those ZoomSnap lands on: 1, 1.5, 2, 3, 4, 6... and 2/3, 1/2, 1/3, 1/4...
[[nodiscard]] float ZoomLevelNext(float zoom, bool in) noexcept {
    float next = in ? kViewportZoomMax : kViewportZoomMin;
    for (int k = -7; k <= 6; ++k) {
        const float power = std::exp2(static_cast<float>(k));
        for (const float level : {power, power * (k < 0 ? 4.0f / 3.0f : 1.5f)}) {
            if (in && level > zoom * 1.01f) {
                next = std::min(next, level);
            } else if (!in && level < zoom / 1.01f) {
                next = std::max(next, level);
            }
        }
    }
    return next;
}

// view.zoomIn, view.zoomOut and view.zoomActual, around the view center; flips and rotation are kept.
void ViewZoom(Window window, ActionId action) noexcept {
    AppState& app = gAppState;
    const CanvasView view = WindowCanvasView(window);
    const auto windowState = WindowGetState(window);
    if (view == kCanvasViewInvalid || !windowState) {
        return;
    }
    Viewport& viewport = app.canvasViews[view].viewport;
    const float zoomX = std::abs(viewport.zoomX);
    const float zoomY = std::abs(viewport.zoomY);
    float amountX = 1.0f / zoomX;
    float amountY = 1.0f / zoomY;
    if (action != ActionId::ViewZoomActual) {
        amountX = ZoomLevelNext(zoomX, action == ActionId::ViewZoomIn) / zoomX;
        amountY = amountX;
    }
    ViewStepBegin(view);
    viewport = ViewportSnap(ViewportZoom(viewport, amountX, amountY, app.zoomMin, app.zoomMax), windowState->dpiScale);
    if (app.navigate.mode != NavigateMode::None) {
        NavigateBegin(app.navigate.view, app.navigate.mode);
    }
}

// A wheel notch over a view zooms around the pointer, keeping the canvas point under it; during a right drag it turns
// the view 15 degrees around the pointer instead, and the pan goes on from there. Up zooms in and turns clockwise.
[[nodiscard]] bool NavigateWheel(const WheelEvent& wheel, bool uiWants) noexcept {
    AppState& app = gAppState;
    const bool turning = app.navigate.mode == NavigateMode::Pan && app.navigate.button == Button::Right;
    const bool busy = (app.stroke.canvas != kCanvasInvalid && !app.stroke.ended && !AppTransforming())
                   || app.transform.drag != TransformDrag::None || app.eyedropper.view != kCanvasViewInvalid
                   || app.tagDrag.view != kCanvasViewInvalid || app.navigate.mode != NavigateMode::None;
    if (wheel.window >= kWindowMax || wheel.deltaY == 0.0f || (!turning && (uiWants || busy))) {
        return turning;
    }
    const CanvasView view = turning ? app.navigate.view : WindowViewUnderPointer(wheel.window);
    const auto windowState = WindowGetState(wheel.window);
    if (view == kCanvasViewInvalid || !windowState || app.canvasViews[view].window != wheel.window) {
        return turning;
    }
    CanvasViewState& state = app.canvasViews[view];
    const auto [x, y] = ViewPointFromPointer(state.rect, windowState->dpiScale, app.pointerX, app.pointerY);
    Viewport viewport = ViewportMove(state.viewport, -x, -y);
    if (turning) {
        // A single flip mirrors the turn on screen.
        const bool mirrored = (viewport.zoomX < 0.0f) != (viewport.zoomY < 0.0f);
        const float turn = wheel.deltaY * kRotateSnap;
        viewport = ViewportRotate(viewport, mirrored ? -turn : turn);
    } else {
        const float amount = std::exp2(wheel.deltaY / kWheelZoomNotches);
        viewport = ViewportZoom(viewport, amount, amount, app.zoomMin, app.zoomMax);
        ViewStepBegin(view);
    }
    state.viewport = ViewportSnap(ViewportMove(viewport, x, y), windowState->dpiScale);
    if (turning) {
        NavigateBegin(view, NavigateMode::Pan);
    }
    return true;
}

// While view.navigate is held: left drag pans, with Ctrl zooms and with Shift rotates, around the view center. Holding
// Alt snaps the zoom (ZoomSnap) and the rotation to 15 degrees. Pressing or releasing Ctrl or Shift mid-drag switches
// mode from where the pointer is. Without the keyboard: right drag pans, and the wheel zooms or, during that drag,
// rotates (NavigateWheel). Returns true when the event belongs to navigation. Releases and focus loss are handled even
// when the UI wants them, so a drag never sticks.
[[nodiscard]] bool Navigate(const Event& event, bool uiWants) noexcept {
    ZoneScopedN("Navigate");
    AppState& app = gAppState;
    switch (event.type) {
        case EventType::Key: {
            const NavigateMode mode = NavigateModeFromMods(app.keyMods);
            if (app.navigate.mode == NavigateMode::None || app.navigate.button != Button::Left) {
                return false;
            }
            if (app.navigate.mode != mode) {
                NavigateBegin(app.navigate.view, mode);
            } else {
                // Alt pressed or released snaps or frees the view right away.
                NavigateUpdate();
            }
            return false;
        }
        case EventType::Button: {
            const Button button = event.button.button;
            if (button != Button::Left && button != Button::Right) {
                return false;
            }
            if (event.button.inputState == InputState::Release && app.navigate.mode != NavigateMode::None
                && app.navigate.button == button) {
                NavigateEnd();
                return true;
            }
            const CanvasView view = WindowCanvasView(event.button.window);
            if (view == kCanvasViewInvalid || event.button.inputState != InputState::Press || uiWants
                || app.navigate.mode != NavigateMode::None) {
                return false;
            }
            if (button == Button::Left && app.spaceDown) {
                NavigateBegin(view, NavigateModeFromMods(app.keyMods));
                return true;
            }
            // Not over a stroke or a drag of another kind; a transform waiting for its next drag lets it pan.
            if (button == Button::Right && !app.spaceDown
                && (app.stroke.canvas == kCanvasInvalid || app.stroke.ended || AppTransforming())
                && app.transform.drag == TransformDrag::None && app.eyedropper.view == kCanvasViewInvalid
                && app.tagDrag.view == kCanvasViewInvalid) {
                NavigateBegin(view, NavigateMode::Pan, Button::Right);
                return true;
            }
            return false;
        }
        case EventType::Wheel:
            return NavigateWheel(event.wheel, uiWants);
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
                NavigateEnd();
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

// brush.smaller and brush.larger: kBrushSizeStep at a time, in whole canvas units from 1 up, as the radius adjust keeps
// them, moving at least one. Not while stroking or adjusting.
void BrushSizeStep(bool larger) noexcept {
    AppState& app = gAppState;
    const std::vector<MdslParam>& params = ToolSource(app.tools, ToolCurrent(app.tools)).params;
    const int param = BrushAdjustParam(params, BrushAdjustMode::Radius);
    if (param < 0 || app.brushAdjust.mode != BrushAdjustMode::None
        || (app.stroke.canvas != kCanvasInvalid && !app.stroke.ended)) {
        return;
    }
    const MdslParam& info = params[static_cast<std::size_t>(param)];
    float& size = ToolCurrent(app.tools).brush.params[static_cast<std::size_t>(param)].value[0];
    float next = larger ? size * kBrushSizeStep : size / kBrushSizeStep;
    if (next >= 1.0f) {
        next = larger ? std::max(std::round(next), std::floor(size) + 1.0f)
                      : std::min(std::round(next), std::ceil(size) - 1.0f);
    }
    size = std::clamp(next, info.min, info.max);
}

// brush.opacitySet, `percent` of full opacity within the param's range. Not while stroking or adjusting.
void BrushOpacitySet(std::uint32_t percent) noexcept {
    AppState& app = gAppState;
    const std::vector<MdslParam>& params = ToolSource(app.tools, ToolCurrent(app.tools)).params;
    const int param = BrushAdjustParam(params, BrushAdjustMode::Opacity);
    if (param < 0 || app.brushAdjust.mode != BrushAdjustMode::None
        || (app.stroke.canvas != kCanvasInvalid && !app.stroke.ended)) {
        return;
    }
    const MdslParam& info = params[static_cast<std::size_t>(param)];
    ToolCurrent(app.tools).brush.params[static_cast<std::size_t>(param)].value[0] =
        std::clamp(static_cast<float>(percent) / 100.0f, info.min, info.max);
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
                BrushAdjustCancel();
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
    if (app.brushAdjust.mode != BrushAdjustMode::None && app.canvasViews[app.brushAdjust.view].window == window) {
        return WindowCursor::Arrow;
    }
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
            return WindowCursor::Arrow;
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
    const CanvasView view = app.stroke.canvas != kCanvasInvalid && app.canvasViews[app.stroke.view].window == window
                              ? app.stroke.view
                              : WindowViewUnderPointer(window);
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
        .smoothing = ToolCurrent(tools).smoothing,
        .smoothingTail = ToolCurrent(tools).smoothingTail,
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
        const CanvasViewRect& rect = gAppState.canvasViews[view].rect;
        const double halfWidth = windowState ? rect.width / windowState->dpiScale * 0.5 : 0.0;
        const double halfHeight = windowState ? rect.height / windowState->dpiScale * 0.5 : 0.0;
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

// Grid positions of the root composites an export wants at once.
constexpr std::size_t kExportWindow = 16;

// A layer `folder` shows, through shown folders, has a tile at `gridPos`: its composite there has content, as
// CompositePrepare decides.
[[nodiscard]] bool FolderShowsTile(const CanvasState& canvas, Layer folder, TileKey gridPos) noexcept {
    const Layer top = canvas.layerHeights[folder];
    const int bottom = static_cast<int>(top) - canvas.layers[folder].descendants;
    for (int h = static_cast<int>(top) - 1; h >= bottom;
         h -= canvas.layers[canvas.layersOrder[static_cast<std::size_t>(h)]].descendants + 1) {
        const Layer child = canvas.layersOrder[static_cast<std::size_t>(h)];
        const LayerState& layer = canvas.layers[child];
        if (HasAnyFlag(layer.flags, LayerFlags::Hidden) || layer.opacity <= 0.0f) {
            continue;
        }
        if (HasAnyFlag(layer.flags, LayerFlags::Folder)
                ? FolderShowsTile(canvas, child, gridPos)
                : canvas.tileIndices.contains(TileGridPosLayerKey(gridPos, child))) {
            return true;
        }
    }
    return false;
}

// The grid positions where a layer in the order has a level-0 tile, as a box; false when there are none.
[[nodiscard]] bool CanvasDrawnGrid(const CanvasState& canvas, TileGridPos& min, TileGridPos& max) noexcept {
    bool any = false;
    for (const TileState& tile : canvas.tiles) {
        const Layer layer = TileKeyLayer(tile.key);
        if (TileKeyMip(tile.key) != 0 || HasAnyFlag(tile.flags, TileFlags::Composite)
            || canvas.layerHeights[layer] == kLayerInvalid
            || HasAnyFlag(canvas.layers[layer].flags, LayerFlags::Mask)) {
            continue;
        }
        const TileGridPos pos = TileKeyGridPos(tile.key);
        min = any ? TileGridPos{.x = std::min(min.x, pos.x), .y = std::min(min.y, pos.y)} : pos;
        max = any ? TileGridPos{.x = std::max(max.x, pos.x), .y = std::max(max.y, pos.y)} : pos;
        any = true;
    }
    return any;
}

// What an export covers: the selection's bounds, times its coverage, or else every tile drawn, cropped to what shows
// once composited. Canvas texels, max exclusive.
struct ExportRegion {
    std::int64_t minX;
    std::int64_t minY;
    std::int64_t maxX;
    std::int64_t maxY;
    bool masked;
};

// False when nothing is drawn and nothing selected.
[[nodiscard]] bool ExportRegionGet(const CanvasState& canvas, ExportRegion& region) noexcept {
    region.masked = canvas.selection != kLayerInvalid && !SelectionEmpty(canvas.selectionBounds);
    if (region.masked) {
        region.minX = static_cast<std::int64_t>(std::floor(canvas.selectionBounds.minX));
        region.minY = static_cast<std::int64_t>(std::floor(canvas.selectionBounds.minY));
        region.maxX = static_cast<std::int64_t>(std::ceil(canvas.selectionBounds.maxX));
        region.maxY = static_cast<std::int64_t>(std::ceil(canvas.selectionBounds.maxY));
        return true;
    }
    TileGridPos min{};
    TileGridPos max{};
    if (!CanvasDrawnGrid(canvas, min, max)) {
        return false;
    }
    region.minX = std::int64_t{min.x} * kTileSize;
    region.minY = std::int64_t{min.y} * kTileSize;
    region.maxX = (std::int64_t{max.x} + 1) * kTileSize;
    region.maxY = (std::int64_t{max.y} + 1) * kTileSize;
    return true;
}

// ExportUpdate then writes `path` over the next updates, the canvas's background colour under it when `background`.
void ExportStart(Window window, const CanvasState& canvas, const ExportRegion& region, std::filesystem::path path,
                 bool background) noexcept {
    const auto tileOf = [](std::int64_t pos) {
        return static_cast<TileAxis>(pos >= 0 ? pos / kTileSize : ((pos + 1) / kTileSize) - 1);
    };
    ExportState& job = gAppState.exporting;
    job = ExportState{
        .path = std::move(path),
        .pixels = std::vector<std::byte>(
            static_cast<std::size_t>((region.maxX - region.minX) * (region.maxY - region.minY) * 4)),
        .remaining = {},
        .x = region.minX,
        .y = region.minY,
        .width = static_cast<std::uint32_t>(region.maxX - region.minX),
        .height = static_cast<std::uint32_t>(region.maxY - region.minY),
        .background = background ? canvas.backgroundColor : Color::Srgb{},
        .canvas = canvas.id,
        .window = window,
        .masked = region.masked,
    };
    for (TileAxis y = tileOf(region.minY); y <= tileOf(region.maxY - 1); ++y) {
        for (TileAxis x = tileOf(region.minX); x <= tileOf(region.maxX - 1); ++x) {
            job.remaining.push_back(TileKeyMake(0, 0, TileGridPos{.x = x, .y = y}));
        }
    }
}

// canvas.export: asks for a PNG file, then exports the ExportRegion. The dialog's check box puts the background colour
// under it, as the view shows it; unchecked, the image keeps its alpha.
void AppExport(Window window) noexcept {
    ZoneScopedN("AppExport");
    if (ActionBlocked(window, ActionId::CanvasExport, 0) != Text::Count_) {
        return;
    }
    const CanvasState& canvas = *WindowCanvas(window);
    ExportRegion region{};
    if (!ExportRegionGet(canvas, region)) {
        return;
    }
    if (region.maxX - region.minX > kPlatformImageSideMax || region.maxY - region.minY > kPlatformImageSideMax) {
        AppError(window, TextFormat(Text::ErrorExport, CanvasName(canvas).c_str()), Error::ImageTooLarge);
        return;
    }
    std::filesystem::path initial =
        canvas.path.empty()
            ? std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(TextGet(Text::Untitled))))
            : canvas.path;
    initial.replace_extension(".png");
    gAppState.modal = true;
    const auto path = DialogFileSave(DialogFileInfo{
        .initial = initial,
        .typeName = TextGet(Text::ExportFileType),
        .extension = "png",
        .check = TextGet(Text::ExportBackground),
        .checked = &gAppState.exportBackground,
        .window = window,
    });
    gAppState.modal = false;
    if (!path) {
        AppError(window, TextGet(Text::ErrorFileDialog), path.error());
        return;
    }
    if (!path->empty()) {
        ExportStart(window, canvas, region, *path, gAppState.exportBackground);
    }
}

// An export's background, premultiplied, from 0 to 255; all 0 for none.
[[nodiscard]] std::array<unsigned, 4> ExportUnder(const Color::Srgb& background) noexcept {
    const auto byte = [](float value) {
        return static_cast<unsigned>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    const float a = background.a;
    return {byte(background.r * a), byte(background.g * a), byte(background.b * a), byte(a)};
}

// A premultiplied pixel over `under`, as the view draws the canvas over its background.
void PixelOver(std::byte* pixel, const std::array<unsigned, 4>& under) noexcept {
    const unsigned rest = 255u - std::to_integer<unsigned>(pixel[3]);
    for (std::size_t channel = 0; channel < 4; ++channel) {
        pixel[channel] =
            static_cast<std::byte>(std::to_integer<unsigned>(pixel[channel]) + (((under[channel] * rest) + 127) / 255));
    }
}

// The export's image cropped to its pixels with any alpha, unless a selection's, the background put under it, then
// written; its errors shown in its window.
void ExportFinish() noexcept {
    ZoneScopedN("ExportFinish");
    ExportState& job = gAppState.exporting;
    PlatformImage image{.pixels = {}, .width = job.width, .height = job.height};
    std::uint32_t left = job.width;
    std::uint32_t top = job.height;
    std::uint32_t right = 0;
    std::uint32_t bottom = 0;
    for (std::uint32_t y = 0; y < job.height; ++y) {
        for (std::uint32_t x = 0; x < job.width; ++x) {
            if (job.pixels[(((std::size_t{y} * job.width) + x) * 4) + 3] != std::byte{0}) {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x + 1);
                bottom = std::max(bottom, y + 1);
            }
        }
    }
    // A selection keeps its bounds; an image with nothing showing stays whole.
    if (job.masked || right == 0) {
        image.pixels = std::move(job.pixels);
    } else {
        image.width = right - left;
        image.height = bottom - top;
        image.pixels.resize(std::size_t{image.width} * image.height * 4);
        for (std::uint32_t y = 0; y < image.height; ++y) {
            std::memcpy(image.pixels.data() + (std::size_t{y} * image.width * 4),
                        job.pixels.data() + (((std::size_t{top + y} * job.width) + left) * 4),
                        std::size_t{image.width} * 4);
        }
    }
    // A selection's took its shape as it was copied.
    if (const std::array<unsigned, 4> under = ExportUnder(job.background); !job.masked && under[3] != 0) {
        for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
            PixelOver(image.pixels.data() + i, under);
        }
    }
    const Window window = WindowGetState(job.window) ? job.window : kWindowInvalid;
    const std::filesystem::path path = job.path;
    job = {};
    if (const auto res = PlatformImageWrite(path, image); !res) {
        AppError(window, TextFormat(Text::ErrorExport, PathUtf8(path.filename()).c_str()), res.error());
    }
}

// Each update, after TilesMarkVisible: the export's next positions whose root composite is current are copied, the
// others wanted, so the next submit composites them. Positions where nothing shows stay transparent.
void ExportUpdate() noexcept {
    ExportState& job = gAppState.exporting;
    if (job.canvas == kCanvasInvalid) {
        return;
    }
    ZoneScopedN("ExportUpdate");
    CanvasState& canvas = gAppState.canvases[job.canvas];
    const Layer root = canvas.layersOrder.back();
    const std::array<unsigned, 4> under = ExportUnder(job.background);
    std::vector<TileKey> done;
    std::vector<TileKey> keys; // per position copied, the root's tile then, masked, the selection's
    std::vector<bool> shown;   // per position copied: false for one only the background covers
    for (std::size_t i = 0; i < job.remaining.size() && i < kExportWindow; ++i) {
        const TileKey gridPos = job.remaining[i];
        if (!FolderShowsTile(canvas, root, gridPos)) {
            done.push_back(gridPos);
            // A selection's background takes its shape here; without one, ExportFinish puts it under all.
            if (job.masked && under[3] != 0) {
                keys.push_back(TileGridPosLayerKey(gridPos, root));
                keys.push_back(TileGridPosLayerKey(gridPos, canvas.selection));
                shown.push_back(false);
            }
            continue;
        }
        const TileKey key = TileGridPosLayerKey(gridPos, root);
        if (const auto found = canvas.tileIndices.find(key); found != canvas.tileIndices.end()) {
            TileState& tile = canvas.tiles[found->second];
            tile.visibleFrame = gAppState.frame;
            if (!HasAnyFlag(tile.flags, TileFlags::Dirty) && tile.cell != kTileAtlasCellInvalid
                && !std::ranges::contains(canvas.composites, key)) {
                done.push_back(gridPos);
                keys.push_back(key);
                if (job.masked) {
                    keys.push_back(TileGridPosLayerKey(gridPos, canvas.selection));
                }
                shown.push_back(true);
                continue;
            }
        }
        canvas.compositeWants.push_back(CompositeWant{.gridPos = gridPos, .level = 0});
    }
    if (!keys.empty()) {
        const std::size_t tileBytes = TileBytes(canvas.pixelFormat);
        std::vector<std::byte> pixels(keys.size() * tileBytes);
        if (const auto res = CanvasTilesGet(canvas, keys, pixels); !res) {
            const Window window = WindowGetState(job.window) ? job.window : kWindowInvalid;
            const std::string name = PathUtf8(job.path.filename());
            job = {};
            AppError(window, TextFormat(Text::ErrorExport, name.c_str()), res.error());
            return;
        }
        constexpr std::int64_t kSize = kTileSize;
        const std::size_t stride = job.masked ? 2 : 1;
        for (std::size_t k = 0; k < keys.size(); k += stride) {
            std::byte* composite = pixels.data() + (k * tileBytes);
            const std::byte* mask = job.masked ? composite + tileBytes : nullptr;
            // A stale tile where nothing shows any more.
            if (!shown[k / stride]) {
                std::fill_n(composite, tileBytes, std::byte{0});
            }
            const TileGridPos pos = TileKeyGridPos(keys[k]);
            const std::int64_t tileX = std::int64_t{pos.x} * kSize;
            const std::int64_t tileY = std::int64_t{pos.y} * kSize;
            const std::int64_t endX = job.x + job.width;
            const std::int64_t endY = job.y + job.height;
            for (std::int64_t y = std::max(job.y, tileY); y < std::min(endY, tileY + kSize); ++y) {
                for (std::int64_t x = std::max(job.x, tileX); x < std::min(endX, tileX + kSize); ++x) {
                    const auto texel = static_cast<std::size_t>((((y - tileY) * kSize) + (x - tileX)) * 4);
                    std::byte* out = job.pixels.data() + ((((y - job.y) * job.width) + (x - job.x)) * 4);
                    std::copy_n(composite + texel, 4, out);
                    if (!mask) {
                        continue;
                    }
                    PixelOver(out, under);
                    const unsigned coverage = std::to_integer<unsigned>(mask[texel + 3]);
                    for (std::size_t channel = 0; channel < 4; ++channel) {
                        out[channel] =
                            static_cast<std::byte>(((std::to_integer<unsigned>(out[channel]) * coverage) + 127) / 255);
                    }
                }
            }
        }
    }
    std::erase_if(job.remaining, [&](TileKey gridPos) { return std::ranges::contains(done, gridPos); });
    if (job.remaining.empty()) {
        ExportFinish();
    }
}

// The benchmark's scene: kBenchmarkLayers layers, each crossed by kBenchmarkStrokes waves over a square of
// kBenchmarkSide canvas units around the origin; then kBenchmarkFrames of zooming in and panning across it.
constexpr std::uint32_t kBenchmarkLayers = 4;
constexpr std::uint32_t kBenchmarkStrokes = 24;
constexpr double kBenchmarkSide = 6144.0;
constexpr std::uint32_t kBenchmarkFrames = 240;
// Updates in a row with nothing left to do before a phase counts as done.
constexpr std::uint32_t kBenchmarkSettleUpdates = 3;

[[nodiscard]] double MillisecondsSince(std::chrono::steady_clock::time_point start) noexcept {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void BenchmarkRecord(std::string_view name, double milliseconds) noexcept {
    gAppState.benchmark.results.push_back(BenchmarkResult{.name = name, .milliseconds = milliseconds});
    MIDORI_LOG_INFO("benchmark: {} {:.1f} ms", name, milliseconds);
}

// What the phase started is done: no stroke, export or work of the app's for kBenchmarkSettleUpdates updates.
[[nodiscard]] bool BenchmarkSettled() noexcept {
    AppState& app = gAppState;
    const bool idle = !app.busy && app.stroke.canvas == kCanvasInvalid && app.exporting.canvas == kCanvasInvalid;
    app.benchmark.idleUpdates = idle ? app.benchmark.idleUpdates + 1 : 0;
    return app.benchmark.idleUpdates >= kBenchmarkSettleUpdates;
}

// The whole scene in the view, at a zoom from it to 400% by `t` from 0 to 1, circling in toward the center and turning.
[[nodiscard]] Viewport BenchmarkViewport(const CanvasViewState& view, float dpiScale, double t) noexcept {
    const double side = std::min(view.rect.width, view.rect.height) / static_cast<double>(dpiScale);
    const double fit = std::max(side, 1.0) / kBenchmarkSide;
    const double radius = kBenchmarkSide * 0.3 * (1.0 - t);
    const double angle = t * 4.0 * std::numbers::pi;
    return Viewport{
        .posX = std::cos(angle) * radius,
        .posY = std::sin(angle) * radius,
        .zoomX = static_cast<float>(fit * std::pow(4.0 / fit, t)),
        .zoomY = static_cast<float>(fit * std::pow(4.0 / fit, t)),
        .rot = static_cast<float>(t * std::numbers::pi / 6.0),
    };
}

// Stroke `index`: a wave across the scene on the layer it falls to, in that layer's hue, with the built-in brush at
// size 48.
void BenchmarkStroke(std::uint32_t index) noexcept {
    const BenchmarkState& bench = gAppState.benchmark;
    CanvasState& canvas = gAppState.canvases[bench.canvas];
    const std::uint32_t layer = index / kBenchmarkStrokes;
    // Bottom first; the root is last.
    canvas.activeLayer = canvas.layersOrder[layer];
    const MdslBrush& source = gAppState.tools.shaders[0].brush;
    BrushState brush{};
    BrushParamsDefault(brush, source);
    if (const int size = BrushParamFind(source.params, "size"); size >= 0) {
        brush.params[static_cast<std::size_t>(size)].value[0] = 48.0f;
    }
    const double half = kBenchmarkSide * 0.5;
    const double row =
        ((static_cast<double>(index % kBenchmarkStrokes) + 0.5) / kBenchmarkStrokes * kBenchmarkSide) - half;
    const double hue = 2.0 * std::numbers::pi * static_cast<double>(layer) / kBenchmarkLayers;
    const auto channel = [hue](double offset) {
        return static_cast<float>(0.5 + (0.5 * std::cos(hue + offset)));
    };
    const auto point = [&](double x) {
        return StrokePoint{
            .x = x,
            .y = row + (std::sin((x / 300.0) + static_cast<double>(layer)) * 120.0),
            .time = (x + half) / kBenchmarkSide,
            .pressure = 0.8f,
            .tiltX = 0.0f,
            .tiltY = 0.0f,
        };
    };
    StrokeBegin(StrokeBeginInfo{
        .brush = brush,
        .color = Color::Srgb{.r = channel(0.0), .g = channel(2.1), .b = channel(4.2), .a = 1.0f},
        .point = point(-half),
        .layer = canvas.activeLayer,
        .view = bench.view,
        .mode = StrokeMode::Paint,
        .blendMode = BlendMode::Normal,
        .shape = StrokeShape::Brush,
        .shader = 0,
    });
    for (double x = -half + 8.0; x <= half; x += 8.0) {
        StrokeTo(point(x));
    }
    StrokeEnd();
}

// debug.benchmark: a new tab with kBenchmarkLayers layers, which BenchmarkUpdate paints, navigates, saves, opens again
// and exports.
void AppBenchmarkStart(Window window) noexcept {
    ZoneScopedN("AppBenchmarkStart");
    AppState& app = gAppState;
    if (ActionBlocked(window, ActionId::DebugBenchmark, 0) != Text::Count_) {
        return;
    }
    if (const auto res = AppCanvasNew(window); !res) {
        MIDORI_LOG_ERROR("benchmark: no canvas: {}", ErrorMessage(res.error()));
        return;
    }
    const CanvasView view = WindowCanvasView(window);
    if (view == kCanvasViewInvalid) {
        return;
    }
    for (std::uint32_t i = 1; i < kBenchmarkLayers; ++i) {
        AppLayerNew(window, false);
    }
    app.benchmark = BenchmarkState{
        .results = {},
        .file = {},
        .phaseStart = std::chrono::steady_clock::now(),
        .frameTime = {},
        .frameMax = 0.0,
        .step = 0,
        .idleUpdates = 0,
        .canvas = app.canvasViews[view].canvas,
        .view = view,
        .window = window,
        .phase = BenchmarkPhase::Paint,
    };
    MIDORI_LOG_INFO("benchmark: started, {} {} {}", MIDORI_VERSION_STRING, MIDORI_PROFILE, MIDORI_COMPILER);
}

// Appended to benchmarks.csv beside preferences.json, a header first in a new file, so runs compare across versions.
void BenchmarkWrite() noexcept {
    const BenchmarkState& bench = gAppState.benchmark;
    const auto settings = PlatformUserSettingsPath();
    if (!settings) {
        return;
    }
    const std::filesystem::path path = *settings / MIDORI_NAME / "benchmarks.csv";
    std::error_code ec;
    const bool header = !std::filesystem::exists(path, ec);
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (header) {
        file << "date,version,profile,compiler";
        for (const BenchmarkResult& result : bench.results) {
            file << ',' << result.name << " (ms)";
        }
        file << '\n';
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    file << std::format("{:%F %T},{},{},{}", now, MIDORI_VERSION_STRING, MIDORI_PROFILE, MIDORI_COMPILER);
    for (const BenchmarkResult& result : bench.results) {
        file << std::format(",{:.1f}", result.milliseconds);
    }
    file << '\n';
    if (!file) {
        MIDORI_LOG_WARN("benchmark: writing {} failed", PathUtf8(path));
    }
}

// The benchmark's tab closed, its files deleted; the results stay for the statistics window.
void BenchmarkEnd(bool completed) noexcept {
    BenchmarkState& bench = gAppState.benchmark;
    if (completed) {
        BenchmarkWrite();
    }
    if (bench.view != kCanvasViewInvalid && gAppState.canvasViews[bench.view].id == bench.view) {
        CanvasViewDetach(bench.view);
    }
    std::error_code ec;
    if (!bench.file.empty()) {
        std::filesystem::remove(bench.file, ec);
        std::filesystem::path png = bench.file;
        std::filesystem::remove(png.replace_extension(".png"), ec);
    }
    bench.file.clear();
    bench.phase = BenchmarkPhase::None;
}

// Each update: one stroke at a time, then a frame of navigation at a time, then the save, open and export, each phase
// timed until the app settles. Stops when its tab or window is closed.
void BenchmarkUpdate() noexcept {
    AppState& app = gAppState;
    BenchmarkState& bench = app.benchmark;
    if (bench.phase == BenchmarkPhase::None) {
        return;
    }
    ZoneScopedN("BenchmarkUpdate");
    const auto windowState = WindowGetState(bench.window);
    if (!windowState || bench.view == kCanvasViewInvalid || app.canvasViews[bench.view].id != bench.view
        || app.canvasViews[bench.view].canvas != bench.canvas) {
        MIDORI_LOG_WARN("benchmark: stopped, its tab or window closed");
        bench.view = kCanvasViewInvalid;
        BenchmarkEnd(false);
        return;
    }
    CanvasViewState& view = app.canvasViews[bench.view];
    switch (bench.phase) {
        case BenchmarkPhase::Paint:
            if (app.stroke.canvas != kCanvasInvalid) {
                return;
            }
            if (bench.step < kBenchmarkLayers * kBenchmarkStrokes) {
                // The built-in brush, the same everywhere, compiles only once a tool picks it, so the benchmark asks
                // for it, and times from its first stroke.
                if (const ToolShader& builtIn = app.tools.shaders[0]; !GraphicsBrushLoaded(0)) {
                    if (!builtIn.error.empty()) {
                        MIDORI_LOG_ERROR("benchmark: the built-in brush does not compile: {}", builtIn.error);
                        BenchmarkEnd(false);
                    } else if (!builtIn.compiling && !builtIn.ready) {
                        BrushShaderLoad(0);
                    }
                    return;
                }
                if (bench.step == 0) {
                    bench.phaseStart = std::chrono::steady_clock::now();
                }
                BenchmarkStroke(bench.step++);
                return;
            }
            if (!BenchmarkSettled()) {
                return;
            }
            BenchmarkRecord("paint", MillisecondsSince(bench.phaseStart));
            bench.phase = BenchmarkPhase::Navigate;
            bench.step = 0;
            bench.phaseStart = std::chrono::steady_clock::now();
            bench.frameTime = bench.phaseStart;
            return;
        case BenchmarkPhase::Navigate: {
            const auto now = std::chrono::steady_clock::now();
            bench.frameMax =
                std::max(bench.frameMax, std::chrono::duration<double, std::milli>(now - bench.frameTime).count());
            bench.frameTime = now;
            if (bench.step < kBenchmarkFrames) {
                const double t = static_cast<double>(bench.step++) / (kBenchmarkFrames - 1);
                view.viewport = ViewportSnap(BenchmarkViewport(view, windowState->dpiScale, t), windowState->dpiScale);
                return;
            }
            if (!BenchmarkSettled()) {
                return;
            }
            BenchmarkRecord("navigate", MillisecondsSince(bench.phaseStart));
            BenchmarkRecord("slowest frame", bench.frameMax);

            std::error_code ec;
            bench.file = std::filesystem::temp_directory_path(ec) / "midori-benchmark.mori";
            auto start = std::chrono::steady_clock::now();
            if (const auto saved = CanvasSaveAs(bench.canvas, bench.file); !saved) {
                MIDORI_LOG_ERROR("benchmark: save failed: {}", ErrorMessage(saved.error()));
                BenchmarkEnd(false);
                return;
            }
            BenchmarkRecord("save", MillisecondsSince(start));
            CanvasViewDetach(bench.view);
            bench.view = kCanvasViewInvalid;
            start = std::chrono::steady_clock::now();
            const auto opened = CanvasOpen(bench.file);
            if (!opened) {
                MIDORI_LOG_ERROR("benchmark: open failed: {}", ErrorMessage(opened.error()));
                BenchmarkEnd(false);
                return;
            }
            BenchmarkRecord("open", MillisecondsSince(start));
            if (const auto shown = WindowShowCanvas(bench.window, *opened); !shown) {
                MIDORI_LOG_ERROR("benchmark: the saved scene not shown: {}", ErrorMessage(shown.error()));
                BenchmarkEnd(false);
                return;
            }
            bench.canvas = *opened;
            bench.view = WindowCanvasView(bench.window);
            app.canvasViews[bench.view].viewport =
                BenchmarkViewport(app.canvasViews[bench.view], windowState->dpiScale, 0.0);
            bench.phase = BenchmarkPhase::Open;
            bench.phaseStart = start;
            return;
        }
        case BenchmarkPhase::Open: {
            if (!BenchmarkSettled()) {
                return;
            }
            BenchmarkRecord("open until shown", MillisecondsSince(bench.phaseStart));
            ExportRegion region{};
            if (!ExportRegionGet(app.canvases[bench.canvas], region)) {
                MIDORI_LOG_ERROR("benchmark: the saved scene holds no painted tile");
                BenchmarkEnd(false);
                return;
            }
            std::filesystem::path png = bench.file;
            ExportStart(bench.window, app.canvases[bench.canvas], region, png.replace_extension(".png"), true);
            bench.phase = BenchmarkPhase::Export;
            bench.phaseStart = std::chrono::steady_clock::now();
            return;
        }
        case BenchmarkPhase::Export:
            if (!BenchmarkSettled()) {
                return;
            }
            BenchmarkRecord("export", MillisecondsSince(bench.phaseStart));
            BenchmarkEnd(true);
            return;
        case BenchmarkPhase::None:
            return;
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
        const TileMip level =
            TileDrawnLevel(canvas, key, TileLevelForZoom(viewState.viewport.zoomX, gAppState.tileLodBias));
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
                    const bool wasModified = gAppState.canvases[stroke.canvas].modified;
                    StrokeEnd();
                    CanvasEdited(stroke.canvas, wasModified);
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
                const bool wasModified = gAppState.canvases[stroke.canvas].modified;
                StrokeEnd();
                CanvasEdited(stroke.canvas, wasModified);
            }
            return false;
        default:
            return false;
    }
}

// window.new: another window showing the focused view's canvas from the same viewport, or a new canvas without one.
[[nodiscard]] Result<void> WindowNewFrom(Window window) noexcept {
    ZoneScopedN("WindowNewFrom");
    const auto created = WindowCreate(WindowCreateInfo{.title = MIDORI_NAME});
    if (!created) {
        MIDORI_LOG_ERROR("WindowCreate failed: {}", ErrorMessage(created.error()));
        return std::unexpected(created.error());
    }
    const CanvasView view = WindowCanvasView(window);
    if (view == kCanvasViewInvalid) {
        return AppCanvasNew(*created);
    }
    const CanvasViewState& from = gAppState.canvasViews[view];
    if (ViewTabAdd(*created, 0, from.canvas, from.viewport) == kCanvasViewInvalid) {
        return std::unexpected(Error::CanvasViewLimitReached);
    }
    return {};
}

// view.splitRight and view.splitDown: the focused pane becomes two, the old one first, the new one showing its canvas
// again from the same viewport, focused.
void ViewSplit(Window window, LayoutSplit split) noexcept {
    const CanvasView view = WindowCanvasView(window);
    WindowLayout& layout = gAppState.layouts[window];
    if (view == kCanvasViewInvalid || layout.count + 2 > kLayoutNodesMax) {
        return;
    }
    const std::uint8_t pane = layout.focused;
    const auto kept = layout.count;
    const auto added = static_cast<std::uint8_t>(layout.count + 1);
    layout.count = static_cast<std::uint8_t>(layout.count + 2);
    layout.nodes[kept] = layout.nodes[pane];
    layout.nodes[added] = LayoutNode{};
    layout.nodes[pane] = LayoutNode{.share = 0.5f, .first = kept, .second = added, .split = split};
    for (CanvasViewState& other : gAppState.canvasViews) {
        if (other.id != kCanvasViewInvalid && other.window == window && other.pane == pane) {
            other.pane = kept;
        }
    }
    layout.focused = kept;
    const CanvasViewState& from = gAppState.canvasViews[view];
    if (ViewTabAdd(window, added, from.canvas, from.viewport) == kCanvasViewInvalid) {
        LayoutPaneCollapse(window, added);
    }
}

// view.nextTab and view.previousTab, wrapping around.
void ViewTabStep(Window window, int step) noexcept {
    const CanvasView view = WindowCanvasView(window);
    if (view == kCanvasViewInvalid) {
        return;
    }
    std::array<CanvasView, kCanvasViewMax> tabs{};
    const auto count = static_cast<int>(AppPaneTabs(window, gAppState.layouts[window].focused, tabs));
    const int at = gAppState.canvasViews[view].tab;
    ViewActivate(tabs[static_cast<std::size_t>((((at + step) % count) + count) % count)]);
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
        case ActionId::CanvasExport:
            AppExport(window);
            return {};
        case ActionId::DebugBenchmark:
            AppBenchmarkStart(window);
            return {};
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
        case ActionId::BrushSmaller:
        case ActionId::BrushLarger:
            BrushSizeStep(action == ActionId::BrushLarger);
            return {};
        case ActionId::BrushOpacitySet:
            BrushOpacitySet(arg);
            return {};
        case ActionId::ViewZoomIn:
        case ActionId::ViewZoomOut:
        case ActionId::ViewZoomActual:
            ViewZoom(window, action);
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
        case ActionId::LayerToggleLocked:
            AppLayerToggleLocked(window);
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
            } else if (const auto language = std::ranges::find(kLanguages, arg, &LanguageInfo::id);
                       language != kLanguages.end()) {
                app.language = language->code;
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
        case ActionId::WindowNew:
            return WindowNewFrom(window);
        case ActionId::ViewDuplicate:
            if (const CanvasView view = WindowCanvasView(window); view != kCanvasViewInvalid) {
                const CanvasViewState& from = app.canvasViews[view];
                if (ViewTabAdd(window, app.layouts[window].focused, from.canvas, from.viewport) == kCanvasViewInvalid) {
                    return std::unexpected(Error::CanvasViewLimitReached);
                }
            }
            return {};
        case ActionId::ViewSplitRight:
        case ActionId::ViewSplitDown:
            ViewSplit(window, action == ActionId::ViewSplitRight ? LayoutSplit::Columns : LayoutSplit::Rows);
            return {};
        case ActionId::ViewClose:
            // The save dialog's message loop may have closed the window meanwhile.
            if (const CanvasView view = WindowCanvasView(window);
                view != kCanvasViewInvalid && CanvasViewReleaseConfirmed(view) && app.canvasViews[view].id == view) {
                CanvasViewDetach(view);
            }
            return {};
        case ActionId::ViewNextTab:
        case ActionId::ViewPreviousTab:
            ViewTabStep(window, action == ActionId::ViewNextTab ? 1 : -1);
            return {};
        case ActionId::ViewActivate:
            if (arg < kCanvasViewMax && app.canvasViews[arg].id != kCanvasViewInvalid
                && app.canvasViews[arg].window == window) {
                ViewActivate(static_cast<CanvasView>(arg));
            }
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
        if (gAppState.navigate.button == Button::Left) {
            NavigateEnd();
        }
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
    if (streaming || app.paste.canvas != kCanvasInvalid || app.exporting.canvas != kCanvasInvalid
        || !app.historyPool.ops.empty()
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

// Where autosaves go, in the app's folder under PlatformUserSettingsPath.
[[nodiscard]] Result<std::filesystem::path> RecoveryFolder() noexcept {
    const auto settings = PlatformUserSettingsPath();
    if (!settings) {
        return std::unexpected(settings.error());
    }
    return *settings / MIDORI_NAME / "recovery";
}

// Every kAutosaveInterval after a key or button, each modified canvas is written whole into the recovery folder, for
// the next launch to offer back after a crash. Waits for a stroke, paste, drag or adjust to end.
void Autosave() noexcept {
    ZoneScopedN("Autosave");
    AppState& app = gAppState;
    const auto now = std::chrono::steady_clock::now();
    if (!app.autosaveInput || now - app.autosaveTime < kAutosaveInterval || app.modal
        || (app.stroke.canvas != kCanvasInvalid && !app.stroke.ended) || app.paste.canvas != kCanvasInvalid
        || app.navigate.mode != NavigateMode::None || app.brushAdjust.mode != BrushAdjustMode::None) {
        return;
    }
    app.autosaveTime = now;
    app.autosaveInput = false;
    const auto folder = RecoveryFolder();
    std::error_code ec;
    if (folder) {
        std::filesystem::create_directories(*folder, ec);
    }
    if (!folder || ec) {
        MIDORI_LOG_ERROR("Autosave: no recovery folder");
        return;
    }
    for (CanvasState& canvas : app.canvases) {
        if (canvas.id == kCanvasInvalid || !canvas.modified) {
            continue;
        }
        // One file per canvas, named apart from those an earlier run left.
        std::filesystem::path backup = canvas.backup;
        if (backup.empty()) {
            const auto stamp = std::chrono::system_clock::now().time_since_epoch() / std::chrono::milliseconds(1);
            backup = *folder / std::format("{}-{}.{}", stamp, canvas.id, kCanvasExtension);
        }
        if (const auto res = CanvasBackup(canvas.id, backup); !res) {
            MIDORI_LOG_WARN("Autosave of {} failed: {}", CanvasName(canvas), ErrorMessage(res.error()));
            app.autosaveInput = true;
            continue;
        }
        canvas.backup = std::move(backup);
    }
}

// An autosave a crash left, and the file it was made from: empty for a canvas never saved.
struct RecoveredFile {
    std::filesystem::path backup;
    std::filesystem::path path;
};

// Asks whether to bring back the autosaves a crash left: Yes returns them, No deletes them, Cancel keeps them for the
// next launch. What an interrupted autosave left is deleted.
[[nodiscard]] std::vector<RecoveredFile> RecoveryAsk() noexcept {
    ZoneScopedN("RecoveryAsk");
    std::vector<RecoveredFile> found;
    const auto folder = RecoveryFolder();
    if (!folder) {
        return found;
    }
    std::error_code ec;
    for (std::filesystem::directory_iterator it(*folder, ec), end; !ec && it != end; it.increment(ec)) {
        const std::filesystem::path& path = it->path();
        if (path.extension() == ".txt") {
            continue;
        }
        if (path.extension() != std::filesystem::path(".").concat(kCanvasExtension)) {
            std::error_code removed;
            std::filesystem::remove(path, removed);
            continue;
        }
        std::ifstream note(CanvasBackupNote(path), std::ios::binary);
        const std::string utf8((std::istreambuf_iterator<char>(note)), std::istreambuf_iterator<char>());
        found.push_back(RecoveredFile{.backup = path, .path = std::u8string(utf8.begin(), utf8.end())});
    }
    if (found.empty()) {
        return found;
    }
    std::ranges::sort(found, {}, &RecoveredFile::backup);
    const std::string instruction = TextFormat(Text::AskRecover, static_cast<unsigned>(found.size()));
    gAppState.modal = true;
    const DialogChoice choice = DialogAsk(DialogAskInfo{
        .title = MIDORI_NAME,
        .instruction = instruction,
        .content = TextGet(Text::AskRecoverContent),
        .yes = TextGet(Text::AskRecoverYes),
        .no = TextGet(Text::AskRecoverNo),
        .window = kWindowInvalid,
    });
    gAppState.modal = false;
    if (choice == DialogChoice::No) {
        for (const RecoveredFile& file : found) {
            CanvasBackupRemove(file.backup);
        }
    }
    if (choice != DialogChoice::Yes) {
        found.clear();
    }
    return found;
}

// An autosave as a tab of `window`, modified, its file only suggested when saving. One that fails to show stays in the
// recovery folder.
void RecoveredOpen(Window window, const RecoveredFile& file) noexcept {
    ZoneScopedN("RecoveredOpen");
    const auto canvas = CanvasOpen(file.backup);
    if (!canvas) {
        AppError(window, TextFormat(Text::ErrorOpen, PathUtf8(file.backup.filename()).c_str()), canvas.error());
        return;
    }
    CanvasState& state = gAppState.canvases[*canvas];
    state.path = file.path;
    state.modified = true;
    if (const auto shown = WindowShowCanvas(window, *canvas); !shown) {
        AppError(window, TextFormat(Text::ErrorOpen, PathUtf8(file.backup.filename()).c_str()), shown.error());
        return;
    }
    state.recovered = file.backup;
    CanvasTitlesUpdate(*canvas);
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
        gAppState.tileLodBias = preferences->tileLodBias;
        gAppState.zoomDefault = preferences->zoomDefault;
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

    CacheLeftoversRemove();
    gAppState.autosaveTime = std::chrono::steady_clock::now();
    const std::vector<RecoveredFile> recovered = RecoveryAsk();

    // Each file of the command line in its own window, else the files open at the last exit as their windows' tabs.
    // Gone files are skipped quietly: only the user's own choice of file is worth an error. A file recovered from its
    // autosave is not opened again from the older one on disk.
    std::vector<OpenFile> files;
    for (std::size_t i = 0; i < initInfo.files.size() && i < kWindowMax; ++i) {
        files.push_back(OpenFile{
            .path = initInfo.files[i], .viewport = {}, .tagsChecked = 0, .window = static_cast<std::uint8_t>(i)});
    }
    if (files.empty() && gAppState.reopenFiles) {
        std::error_code ec;
        for (const OpenFile& file : gAppState.openFiles) {
            const bool isRecovered = std::ranges::any_of(recovered, [&](const RecoveredFile& r) {
                return !r.path.empty() && std::filesystem::equivalent(r.path, file.path, ec);
            });
            if (std::filesystem::is_regular_file(file.path, ec) && !isRecovered) {
                files.push_back(file);
            }
        }
    }
    gAppState.openFiles.clear();

    // A window left without a tab, its files failing to open or none given, gets a new canvas. The platform owns the
    // windows and destroys them in PlatformTerminate.
    Window window = kWindowInvalid;
    const auto ensureCanvas = [&window]() -> Result<void> {
        if (window == kWindowInvalid || WindowCanvasView(window) != kCanvasViewInvalid) {
            return {};
        }
        if (const auto res = AppCanvasNew(window); !res) {
            MIDORI_LOG_ERROR("AppCanvasNew failed: {}", ErrorMessage(res.error()));
            return res;
        }
        return {};
    };
    for (std::size_t i = 0; i < std::max<std::size_t>(files.size(), 1); ++i) {
        if (i == 0 || files[i].window != files[i - 1].window) {
            if (const auto res = ensureCanvas(); !res) {
                return res;
            }
            // The first where the last one was left, the others where the system puts them.
            WindowCreateInfo createInfo = i == 0 ? gAppState.windowPlacement : WindowCreateInfo{};
            createInfo.title = MIDORI_NAME;
            const auto created = WindowCreate(createInfo);
            if (!created) {
                MIDORI_LOG_ERROR("WindowCreate failed: {}", ErrorMessage(created.error()));
                // Files already shown keep the app running; without any window it cannot start.
                if (i == 0) {
                    return std::unexpected(created.error());
                }
                break;
            }
            window = *created;
        }
        if (i < files.size() && WindowOpenFile(window, files[i].path)) {
            if (CanvasState* canvas = WindowCanvas(window)) {
                canvas->tagsChecked = files[i].tagsChecked & canvas->tags;
            }
            if (const CanvasView view = WindowCanvasView(window); view != kCanvasViewInvalid) {
                gAppState.canvasViews[view].viewport = files[i].viewport;
            }
        }
    }
    for (const RecoveredFile& file : recovered) {
        RecoveredOpen(window, file);
    }
    return ensureCanvas();
}

Result<void> AppEvent(Event event) noexcept {
    ZoneScopedN("AppEvent");
    // Only what keeps the windows consistent gets through while a dialog is up.
    if (gAppState.modal && event.type != EventType::WindowResize && event.type != EventType::WindowFocus
        && event.type != EventType::WindowDestroy) {
        return {};
    }
    if (event.type == EventType::Key || event.type == EventType::Button) {
        gAppState.autosaveInput = true;
    }
    if (event.type == EventType::Key) {
        gAppState.keyMods = event.key.keyMods;
    } else if (event.type == EventType::WindowFocus && !event.windowFocus.focused) {
        // Keys held are released without an event.
        gAppState.keyMods = KeyMod::None;
        gAppState.spaceDown = false;
        gAppState.pointerPressure = 0.0f;
        gAppState.pointerTiltX = 0.0f;
        gAppState.pointerTiltY = 0.0f;
        gAppState.viewStepView = kCanvasViewInvalid;
        gAppState.keymap.holding = -1;
        gAppState.keymap.repeating = -1;
        gAppState.keymap.pendingLength = 0;
        gAppState.keymap.down.reset();
    } else if (event.type == EventType::Pointer) {
        gAppState.pointerTime = event.pointer.time;
        gAppState.pointerX = event.pointer.posX;
        gAppState.pointerY = event.pointer.posY;
        gAppState.pointerPressure = event.pointer.pressure;
        gAppState.pointerTiltX = event.pointer.tiltX;
        gAppState.pointerTiltY = event.pointer.tiltY;
    }
    const bool uiWants = UiEvent(event);
    // A press over a pane focuses it first, so what the press starts acts on the view it shows. Not while a drag of any
    // kind goes on, whose release may come over another pane.
    if (event.type == EventType::Button && event.button.inputState == InputState::Press && !uiWants
        && event.button.window < kWindowMax && gAppState.navigate.mode == NavigateMode::None
        && gAppState.brushAdjust.mode == BrushAdjustMode::None
        && (gAppState.stroke.canvas == kCanvasInvalid || gAppState.stroke.ended) && !AppTransforming()
        && gAppState.eyedropper.view == kCanvasViewInvalid && gAppState.tagDrag.view == kCanvasViewInvalid) {
        WindowLayout& layout = gAppState.layouts[event.button.window];
        const std::uint8_t pane = LayoutPaneAt(event.button.window, gAppState.pointerX, gAppState.pointerY);
        if (pane != layout.focused && layout.nodes[pane].active != kCanvasViewInvalid) {
            ViewActivate(layout.nodes[pane].active);
        }
    }
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
            if (event.windowDestroy.window < kWindowMax) {
                gAppState.layouts[event.windowDestroy.window] = {};
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
    AppLayoutUpdate(window);
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
    ExportUpdate();
    BenchmarkUpdate();
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
    Autosave();
    // A dialog an action opens may close windows, so each request checks its own.
    for (const ActionRequest& request : std::exchange(gAppState.actionRequests, {})) {
        if (!WindowGetState(request.window)) {
            continue;
        }
        if (const auto res = ActionRun(request.window, request.action, request.arg); !res) {
            return res;
        }
    }
    // Otherwise a window presents only after input, see UiWantsFrame, and sleeps in between. A benchmark keeps every
    // frame coming, but is no work: it waits for the rest to be done.
    gAppState.busy = AppBusy(streaming);
    if (gAppState.busy || gAppState.benchmark.phase != BenchmarkPhase::None) {
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
        .tileLodBias = gAppState.tileLodBias,
        .zoomDefault = gAppState.zoomDefault,
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
    const auto canvas = CanvasNew();
    if (!canvas) {
        AppError(window, TextGet(Text::ErrorCreate), canvas.error());
        return std::unexpected(canvas.error());
    }
    if (const auto shown = WindowShowCanvas(window, *canvas); !shown) {
        AppError(window, TextGet(Text::ErrorCreate), shown.error());
        return shown;
    }
    return {};
}

Result<void> AppCanvasOpenRecent(Window window, std::size_t index) noexcept {
    ZoneScopedN("AppCanvasOpenRecent");
    if (index >= gAppState.recentFiles.size()) {
        return {};
    }
    // Copied: opening reorders the list.
    const std::filesystem::path path = gAppState.recentFiles[index];
    const auto opened = WindowOpenFile(window, path);
    if (!opened) {
        std::erase(gAppState.recentFiles, path);
    }
    return opened;
}

Result<void> AppCanvasOpen(Window window) noexcept {
    ZoneScopedN("AppCanvasOpen");
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
        case ActionId::CanvasExport: {
            const Text blocked = needsEdit();
            if (blocked != Text::Count_) {
                return blocked;
            }
            if (app.exporting.canvas != kCanvasInvalid) {
                return Text::ReasonExporting;
            }
            TileGridPos min{};
            TileGridPos max{};
            const bool selected = canvas->selection != kLayerInvalid && !SelectionEmpty(canvas->selectionBounds);
            return selected || CanvasDrawnGrid(*canvas, min, max) ? Text::Count_ : Text::ReasonNothingDrawn;
        }
        case ActionId::DebugBenchmark:
            if (app.benchmark.phase != BenchmarkPhase::None) {
                return Text::ReasonBenchmarking;
            }
            return app.exporting.canvas != kCanvasInvalid ? Text::ReasonExporting : Text::Count_;
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
        case ActionId::BrushSmaller:
        case ActionId::BrushLarger:
        case ActionId::BrushOpacitySet:
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
        case ActionId::LayerToggleLocked:
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
        case ActionId::ViewZoomIn:
        case ActionId::ViewZoomOut:
        case ActionId::ViewZoomActual:
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
        case ActionId::ViewDuplicate:
        case ActionId::ViewSplitRight:
        case ActionId::ViewSplitDown:
        case ActionId::ViewClose:
        case ActionId::ViewNextTab:
        case ActionId::ViewPreviousTab:
            return needsCanvas();
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
    if (!state) {
        return kViewportZoomMin;
    }
    const CanvasView view = WindowCanvasView(window);
    const CanvasViewRect rect = view == kCanvasViewInvalid
                                  ? CanvasViewRect{.x = 0, .y = 0, .width = state->width, .height = state->height}
                                  : gAppState.canvasViews[view].rect;
    return TileZoomMin(static_cast<float>(rect.width) / state->dpiScale,
                       static_cast<float>(rect.height) / state->dpiScale, gAppState.tileLodBias);
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

CanvasView AppWindowView(Window window) noexcept {
    if (window >= kWindowMax) {
        return kCanvasViewInvalid;
    }
    const WindowLayout& layout = gAppState.layouts[window];
    return layout.nodes[layout.focused].active;
}

std::uint32_t AppPaneTabs(Window window, std::uint8_t pane, std::span<CanvasView, kCanvasViewMax> tabs) noexcept {
    std::uint32_t count = 0;
    for (const CanvasViewState& view : gAppState.canvasViews) {
        if (view.id != kCanvasViewInvalid && view.window == window && view.pane == pane) {
            tabs[count++] = view.id;
        }
    }
    std::ranges::sort(tabs.first(count), {}, [](CanvasView view) { return gAppState.canvasViews[view].tab; });
    return count;
}

std::string AppCanvasTitle(Canvas canvas) noexcept {
    const CanvasState& state = gAppState.canvases[canvas];
    return state.modified ? CanvasName(state) + "*" : CanvasName(state);
}

void AppLayoutUpdate(Window window) noexcept {
    ZoneScopedN("AppLayoutUpdate");
    const auto state = WindowGetState(window);
    if (!state) {
        return;
    }
    for (CanvasViewState& view : gAppState.canvasViews) {
        if (view.id != kCanvasViewInvalid && view.window == window) {
            view.rect = {};
        }
    }
    // One pane of one tab covers the whole client area as a lone view always did, under the docked panels too.
    const WindowLayout& layout = gAppState.layouts[window];
    std::array<CanvasView, kCanvasViewMax> tabs{};
    const bool divided = layout.count > 1 || AppPaneTabs(window, 0, tabs) > 1;
    const CanvasViewRect& area = layout.area;
    LayoutLay(window, state->dpiScale, 0,
              divided && area.width > 0 && area.height > 0
                  ? area
                  : CanvasViewRect{.x = 0, .y = 0, .width = state->width, .height = state->height});
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

void AppLayerToggleLocked(Window window) noexcept {
    ZoneScopedN("AppLayerToggleLocked");
    CanvasState* canvas = WindowCanvas(window);
    if (!canvas) {
        return;
    }
    const std::vector<Layer> layers = LayersSelected(*canvas);
    if (layers.empty()) {
        return;
    }
    // All the same way, the opposite of the top one.
    const bool locked = !LayerPropertiesGet(*canvas, layers.front()).locked;
    std::vector<LayerProperties> after;
    for (const Layer layer : layers) {
        after.push_back(LayerPropertiesGet(*canvas, layer));
        after.back().locked = locked;
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
    // Each canvas no other window shows, its tab brought up while asked.
    for (const CanvasViewState& view : gAppState.canvasViews) {
        if (view.id == kCanvasViewInvalid || view.window != window || !gAppState.canvases[view.canvas].modified
            || CanvasShownOutside(view.canvas, window)
            || std::ranges::any_of(gAppState.canvasViews.begin(), gAppState.canvasViews.begin() + view.id,
                                   [&view](const CanvasViewState& before) {
                                       return before.id != kCanvasViewInvalid && before.window == view.window
                                           && before.canvas == view.canvas;
                                   })) {
            continue;
        }
        ViewActivate(view.id);
        switch (AppAskSave(window, CanvasName(gAppState.canvases[view.canvas]))) {
            case DialogChoice::Yes:
                if (const auto saved = CanvasSaveWithDialogs(window, view.canvas, false); !saved || !*saved) {
                    return {};
                }
                break;
            case DialogChoice::No:
                break;
            case DialogChoice::Cancel:
                return {};
        }
    }
    // Closing the last window ends the app, with no window left to ask at exit.
    WindowPlacementKeep(window);
    std::vector<OpenFile>& openFiles = gAppState.openFiles;
    const auto group = static_cast<std::uint8_t>(openFiles.empty() ? 0 : openFiles.back().window + 1);
    for (std::uint8_t pane = 0; pane < gAppState.layouts[window].count; ++pane) {
        std::array<CanvasView, kCanvasViewMax> tabs{};
        const std::uint32_t count = AppPaneTabs(window, pane, tabs);
        for (std::uint32_t i = 0; i < count; ++i) {
            const CanvasViewState& view = gAppState.canvasViews[tabs[i]];
            const CanvasState& canvas = gAppState.canvases[view.canvas];
            if (!canvas.path.empty() && !std::ranges::contains(openFiles, canvas.path, &OpenFile::path)
                && openFiles.size() < kCanvasViewMax && group < kWindowMax) {
                openFiles.push_back(OpenFile{.path = canvas.path,
                                             .viewport = view.viewport,
                                             .tagsChecked = canvas.tagsChecked,
                                             .window = group});
            }
        }
    }
    return WindowDestroy(window);
}

} // namespace Midori
