#include "midori/app.hpp"
#include "midori/graphics.hpp"
#include "midori/log.hpp"
#include "midori/ui.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include <tracy/Tracy.hpp>

#include "midori/version.h"

namespace Midori {

AppState gAppState{};

namespace {

// Pointer travel, in DIPs, that doubles or halves the zoom.
constexpr float kZoomDragDips = 200.0f;

// Every window has one view over its whole client area for now; with several, this becomes a hit test.
[[nodiscard]] CanvasView WindowCanvasView(Window window) noexcept {
    for (const CanvasViewState& view : gAppState.canvasViews) {
        if (view.id != kCanvasViewInvalid && view.window == window) {
            return view.id;
        }
    }
    return kCanvasViewInvalid;
}

// In place: a CanvasState is too large for a temporary on the stack.
void CanvasReset(CanvasState& canvas) noexcept {
    std::destroy_at(&canvas);
    std::construct_at(&canvas);
}

void CanvasViewDetach(CanvasView view) noexcept {
    CanvasViewState& viewState = gAppState.canvasViews[view];
    const Canvas canvas = viewState.canvas;
    viewState = {};
    if (gAppState.navigate.view == view) {
        gAppState.navigate = {};
    }
    // No files yet: a canvas nobody can see is gone.
    if (std::ranges::none_of(gAppState.canvasViews, [canvas](const CanvasViewState& other) {
            return other.id != kCanvasViewInvalid && other.canvas == canvas;
        })) {
        CanvasReset(gAppState.canvases[canvas]);
    }
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
    // View DIPs around the view center, floored to a whole pixel like the renderer's projection.
    const float centerX = std::floor(static_cast<float>(windowState->width) * 0.5f);
    const float centerY = std::floor(static_cast<float>(windowState->height) * 0.5f);
    const float scale = windowState->dpiScale;
    const float startX = (navigate.pointerStartX - centerX) / scale;
    const float startY = (navigate.pointerStartY - centerY) / scale;
    const float x = (gAppState.pointerX - centerX) / scale;
    const float y = (gAppState.pointerY - centerY) / scale;
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

} // namespace

Result<void> AppInit(AppInitInfo /*initInfo*/) noexcept {
    ZoneScopedN("AppInit");
    MIDORI_LOG_INFO("{} v{} {}-{} {}", MIDORI_NAME, MIDORI_VERSION_STRING, MIDORI_COMPILER, MIDORI_ARCHITECTURE,
                    MIDORI_PROFILE);

    // The platform owns the window and destroys it in PlatformTerminate.
    const auto window = WindowCreate(WindowCreateInfo{.title = MIDORI_NAME " 1"});
    if (!window) {
        MIDORI_LOG_ERROR("WindowCreate failed: {}", ErrorMessage(window.error()));
        return std::unexpected(window.error());
    }
    if (const auto res = AppCanvasNew(*window); !res) {
        MIDORI_LOG_ERROR("AppCanvasNew failed: {}", ErrorMessage(res.error()));
        return std::unexpected(res.error());
    }

    // if (const auto res = WindowCreate(WindowCreateInfo{.title = MIDORI_NAME " 2"}); !res) {
    //     MIDORI_LOG_ERROR("WindowCreate failed: {}", ErrorMessage(res.error()));
    //     return std::unexpected(res.error());
    // }

    return {};
}

Result<void> AppEvent(Event event) noexcept {
    ZoneScopedN("AppEvent");
    const bool uiWants = UiEvent(event);
    if (Navigate(event, uiWants) || uiWants) {
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
    Viewport* viewport = view != kCanvasViewInvalid ? &gAppState.canvasViews[view].viewport : nullptr;
    UiWindowFrame(window, viewport);
    // ImGui animates and trickles queued input over several frames, so keep presenting while it is shown.
    GraphicsWindowRequestFrame(window);
}

void AppTerminate() noexcept {}

Result<void> AppCanvasNew(Window window) noexcept {
    ZoneScopedN("AppCanvasNew");
    AppState& app = gAppState;
    const auto canvasIt = std::ranges::find(app.canvases, kCanvasInvalid, &CanvasState::id);
    if (canvasIt == app.canvases.end()) {
        MIDORI_LOG_ERROR("AppCanvasNew: all {} canvases are in use", kCanvasMax);
        return std::unexpected(Error::CanvasLimitReached);
    }
    const auto viewIt = std::ranges::find(app.canvasViews, kCanvasViewInvalid, &CanvasViewState::id);
    if (viewIt == app.canvasViews.end()) {
        MIDORI_LOG_ERROR("AppCanvasNew: all {} canvas views are in use", kCanvasViewMax);
        return std::unexpected(Error::CanvasViewLimitReached);
    }
    const auto canvas = static_cast<Canvas>(canvasIt - app.canvases.begin());
    const auto view = static_cast<CanvasView>(viewIt - app.canvasViews.begin());

    canvasIt->id = canvas;
    *viewIt = CanvasViewState{.viewport = {}, .id = view, .canvas = canvas, .window = window};
    return {};
}

} // namespace Midori
