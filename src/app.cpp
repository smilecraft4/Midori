#include "midori/app.hpp"
#include "midori/graphics.hpp"
#include "midori/log.hpp"
#include "midori/stroke.hpp"
#include "midori/tile.hpp"
#include "midori/ui.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

#include <tracy/Tracy.hpp>

#include "midori/version.h"

namespace Midori {

AppState gAppState{};

namespace {

// Pointer travel, in DIPs, that doubles or halves the zoom.
constexpr float kZoomDragDips = 200.0f;

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
    CanvasViewState& viewState = gAppState.canvasViews[view];
    const Canvas canvas = viewState.canvas;
    viewState = {};
    if (gAppState.navigate.view == view) {
        gAppState.navigate = {};
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

// The window's view shows `canvas` from now on, with a fresh viewport.
void WindowShowCanvas(Window window, Canvas canvas) noexcept {
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
    const std::string content = std::format("Reason: {}.", ErrorMessage(error));
    gAppState.modal = true;
    DialogError(window, MIDORI_NAME, instruction, content);
    gAppState.modal = false;
}

[[nodiscard]] Result<std::filesystem::path> AppFileDialog(Window window, bool save,
                                                          const std::filesystem::path& initial) noexcept {
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

// Ctrl+N new, Ctrl+O open, Ctrl+S save, Ctrl+Shift+S save as. B paints, E erases.
[[nodiscard]] Result<void> KeyShortcut(const KeyEvent& key) noexcept {
    if (key.inputState != InputState::Press) {
        return {};
    }
    if (key.keyMods == KeyMod::None) {
        switch (key.key) {
            case Key::B:
                return AppPaintMode();
            case Key::E:
                return AppEraseMode();
            default:
                return {};
        }
    }
    if (!HasAnyFlag(key.keyMods, KeyMod::Ctrl)) {
        return {};
    }
    switch (key.key) {
        case Key::N:
            return AppCanvasNew(key.window);
        case Key::O:
            return AppCanvasOpen(key.window);
        case Key::S:
            return HasAnyFlag(key.keyMods, KeyMod::Shift) ? AppCanvasSaveAs(key.window) : AppCanvasSave(key.window);
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

void NavigateBegin(CanvasView view, NavigateMode mode) noexcept {
    gAppState.navigate = NavigateState{
        .viewportStart = gAppState.canvasViews[view].viewport,
        .pointerStartX = gAppState.pointerX,
        .pointerStartY = gAppState.pointerY,
        .view = view,
        .mode = mode,
    };
}

void NavigateUpdate() noexcept {
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
            const float amount = std::exp2(((x - startX) - (y - startY)) / kZoomDragDips);
            view.viewport = ViewportZoom(start, amount, amount);
            break;
        }
        case NavigateMode::Rotate: {
            // A single flip mirrors the turn on screen, so it is reversed to keep following the pointer.
            const bool mirrored = (start.zoomX < 0.0f) != (start.zoomY < 0.0f);
            const float turn = std::atan2(y, x) - std::atan2(startY, startX);
            view.viewport = ViewportRotate(start, mirrored ? -turn : turn);
            break;
        }
        case NavigateMode::None:
            break;
    }
}

void NavigateEnd() noexcept {
    gAppState.navigate = {};
}

// Space + left drag pans, Ctrl+Space + drag zooms and Shift+Space + drag rotates, around the view center.
// Pressing or releasing Ctrl or Shift mid-drag switches mode from where the pointer is. Returns true when
// the event belongs to navigation. Releases and focus loss are handled even when the UI wants them, so a
// drag never sticks.
[[nodiscard]] bool Navigate(const Event& event, bool uiWants) noexcept {
    AppState& app = gAppState;
    switch (event.type) {
        case EventType::Key: {
            app.keyMods = event.key.keyMods;
            const bool space = event.key.key == Key::Space;
            const bool wasDown = app.spaceDown;
            if (space && event.key.inputState == InputState::Press && !uiWants) {
                app.spaceDown = true;
            } else if (space && event.key.inputState == InputState::Release) {
                app.spaceDown = false;
                NavigateEnd();
            }
            const NavigateMode mode = NavigateModeFromMods(app.keyMods);
            if (app.navigate.mode != NavigateMode::None && app.navigate.mode != mode) {
                NavigateBegin(app.navigate.view, mode);
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
            app.pointerX = event.pointer.posX;
            app.pointerY = event.pointer.posY;
            app.pointerPressure = event.pointer.pressure;
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

// The titles show the * once an edit modified the canvas.
void CanvasEdited(Canvas canvas, bool wasModified) noexcept {
    if (!wasModified && gAppState.canvases[canvas].modified) {
        CanvasTitlesUpdate(canvas);
    }
}

[[nodiscard]] StrokeBeginInfo StrokeBeginInfoFromTool(CanvasView view, StrokePoint point) noexcept {
    const ToolsState& tools = gAppState.tools;
    if (tools.tool == Tool::Erase) {
        return StrokeBeginInfo{
            .brush = tools.erase.brush,
            .color = tools.erase.color,
            .point = point,
            .layer = 0,
            .view = view,
            .mode = StrokeMode::Erase,
            .blendMode = BlendMode::Normal,
        };
    }
    return StrokeBeginInfo{
        .brush = tools.paint.brush,
        .color = tools.paint.color,
        .point = point,
        .layer = 0,
        .view = view,
        .mode = StrokeMode::Paint,
        .blendMode = tools.paint.blendMode,
    };
}

// Left drag paints or erases layer 0, by the current tool. Releases and focus loss end the stroke even when the UI
// wants them. Returns true when the event belongs to the stroke.
[[nodiscard]] bool Paint(const Event& event, bool uiWants) noexcept {
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
            // An ended stroke the GPU has not finished yet blocks the next one for a frame at most.
            if (event.button.inputState != InputState::Press || uiWants || view == kCanvasViewInvalid
                || stroke.canvas != kCanvasInvalid) {
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

// Right press creates the layer 0 tile under the pointer, or deletes it.
[[nodiscard]] Result<void> TileEdit(const ButtonEvent& button) noexcept {
    if (button.inputState != InputState::Press || button.button != Button::Right
        || gAppState.stroke.canvas != kCanvasInvalid) {
        return {};
    }
    const CanvasView view = WindowCanvasView(button.window);
    if (view == kCanvasViewInvalid) {
        return {};
    }
    const auto point = CanvasPointUnderPointer(view, 0.0f);
    if (!point) {
        return std::unexpected(point.error());
    }
    const Tile tile{
        .canvas = gAppState.canvasViews[view].canvas,
        .mip = 0,
        .layer = 0,
        .gridPos = {.x = static_cast<TileAxis>(std::floor(point->x / static_cast<float>(kTileSize))),
                    .y = static_cast<TileAxis>(std::floor(point->y / static_cast<float>(kTileSize)))},
    };
    const CanvasState& canvas = gAppState.canvases[tile.canvas];
    const bool wasModified = canvas.modified;
    const bool exists = canvas.tileIndices.contains(TileKeyMake(tile.layer, tile.mip, tile.gridPos));
    const auto res = exists ? TileDelete(tile) : TileNew(tile);
    CanvasEdited(tile.canvas, wasModified);
    return res;
}

} // namespace

Result<void> AppInit(AppInitInfo initInfo) noexcept {
    ZoneScopedN("AppInit");
    MIDORI_LOG_INFO("{} v{} {}-{} {}", MIDORI_NAME, MIDORI_VERSION_STRING, MIDORI_COMPILER, MIDORI_ARCHITECTURE,
                    MIDORI_PROFILE);

    gAppState.freeAtlasCells.reserve(kTileAtlasCellMax);
    for (TileAtlasCell cell = kTileAtlasCellMax; cell-- > 0;) {
        if (cell != kTileAtlasCellPattern) {
            gAppState.freeAtlasCells.push_back(cell);
        }
    }

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
    const bool uiWants = UiEvent(event);
    if (Navigate(event, uiWants) || Paint(event, uiWants) || uiWants) {
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
            if (const auto res = TileEdit(event.button); !res) {
                MIDORI_LOG_ERROR("TileEdit failed: {}", ErrorMessage(res.error()));
            }
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
    Viewport* viewport = view != kCanvasViewInvalid ? &gAppState.canvasViews[view].viewport : nullptr;
    if (const UiCommand command = UiWindowFrame(window, viewport, gAppState.backgroundOpacity, gAppState.tools);
        command != UiCommand::None) {
        gAppState.command = command;
        gAppState.commandWindow = window;
    }
    // ImGui animates and trickles queued input over several frames, so keep presenting while it is shown.
    GraphicsWindowRequestFrame(window);
}

Result<void> AppUpdate() noexcept {
    ZoneScopedN("AppUpdate");
    StrokeUpdate();
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

// A stroke in progress keeps the tool it began with.
Result<void> AppPaintMode() noexcept {
    gAppState.tools.tool = Tool::Paint;
    return {};
}

Result<void> AppEraseMode() noexcept {
    gAppState.tools.tool = Tool::Erase;
    return {};
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
