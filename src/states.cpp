#include "states.h"

#include "app.h"
#include <backends/imgui_impl_sdl3.h>
#include <imgui.h>
#include <memory>

namespace Midori {

namespace {

// Collapse left/right variants so a shortcut works with either key.
SDL_Keymod NormalizeMods(const SDL_Keymod mod) {
    SDL_Keymod out = SDL_KMOD_NONE;
    if ((mod & SDL_KMOD_CTRL) != 0u) {
        out |= SDL_KMOD_CTRL;
    }
    if ((mod & SDL_KMOD_SHIFT) != 0u) {
        out |= SDL_KMOD_SHIFT;
    }
    if ((mod & SDL_KMOD_ALT) != 0u) {
        out |= SDL_KMOD_ALT;
    }
    if ((mod & SDL_KMOD_GUI) != 0u) {
        out |= SDL_KMOD_GUI;
    }
    return out;
}

bool IsKeyPressed(const SDL_Event* event, const SDL_Keycode key, const SDL_Keymod mods) {
    return event->type == SDL_EVENT_KEY_DOWN && event->key.key == key && NormalizeMods(event->key.mod) == mods;
}

// While a pen is in range SDL synthesizes mouse events for it; use only the pen events then.
bool IsPointerDown(const App* app, const SDL_Event* event) {
    if (event->type == SDL_EVENT_PEN_DOWN) {
        return true;
    }
    return !app->penInRange && event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT;
}

bool IsPointerUp(const App* app, const SDL_Event* event) {
    if (event->type == SDL_EVENT_PEN_UP) {
        return true;
    }
    return !app->penInRange && event->type == SDL_EVENT_MOUSE_BUTTON_UP && event->button.button == SDL_BUTTON_LEFT;
}

bool IsPointerMove(const App* app, const SDL_Event* event) {
    if (event->type == SDL_EVENT_PEN_MOTION) {
        return true;
    }
    return !app->penInRange && event->type == SDL_EVENT_MOUSE_MOTION;
}

Canvas::StrokePoint MakeStrokePoint(App* app, const bool erasing) {
    if (erasing) {
        const auto& options = app->canvas.eraserOptions;
        return Canvas::StrokePoint{
            .color = glm::vec4(0.0f, 0.0f, 0.0f, options.opacity),
            .position = app->canvas.viewport.ScreenToCanvas(app->cursorCurrentPos),
            .radius = options.radius,
            .flow = options.flow,
            .hardness = options.hardness,
        };
    }
    const auto& options = app->canvas.brushOptions;
    return Canvas::StrokePoint{
        .color = options.color,
        .position = app->canvas.viewport.ScreenToCanvas(app->cursorCurrentPos),
        .radius = options.radius,
        .flow = options.flow,
        .hardness = options.hardness,
    };
}

} // namespace

IState::IState(App* app) : app(app) {}

StateManager::StateManager(App* app) : app(app), current(std::make_unique<IdleState>(app)) {}

void StateManager::Switch(std::unique_ptr<IState> state) {
    next = std::move(state);
}

void StateManager::OnEvent(const SDL_Event* event) {
    ImGui_ImplSDL3_ProcessEvent(event);

    const ImGuiIO& io = ImGui::GetIO();
    // WantCaptureKeyboard is already true when a UI window merely has nav focus (as on startup),
    // which would swallow global shortcuts. Only yield the keyboard while typing in a text field.
    if (io.WantTextInput) {
        if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) {
            return;
        }
    }
    if (io.WantCaptureMouse && !current->CapturesPointer()) {
        switch (event->type) {
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_WHEEL:
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_PEN_DOWN:
            case SDL_EVENT_PEN_UP:
            case SDL_EVENT_PEN_MOTION:
                return;
            default:
                break;
        }
    }

    // Pen tracking
    if (event->type == SDL_EVENT_PEN_PROXIMITY_IN) {
        app->penInRange = true;
    }
    if (event->type == SDL_EVENT_PEN_PROXIMITY_OUT) {
        app->penInRange = false;
    }
    if (event->type == SDL_EVENT_PEN_AXIS && event->paxis.axis == SDL_PEN_AXIS_PRESSURE) {
        app->penPressure = event->paxis.value;
    }

    // Cursor tracking; pen events win over the mouse events SDL synthesizes from them
    if (event->type == SDL_EVENT_MOUSE_MOTION && !app->penInRange) {
        app->cursorLastPos = app->cursorCurrentPos;
        app->cursorCurrentPos = glm::vec2(event->motion.x, event->motion.y);
        app->cursorDeltaPos = app->cursorCurrentPos - app->cursorLastPos;
    }
    if (event->type == SDL_EVENT_PEN_MOTION) {
        app->cursorLastPos = app->cursorCurrentPos;
        app->cursorCurrentPos = glm::vec2(event->pmotion.x, event->pmotion.y);
        app->cursorDeltaPos = app->cursorCurrentPos - app->cursorLastPos;
    }

    // Modifier tracking
    if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) {
        const bool down = event->type == SDL_EVENT_KEY_DOWN;
        switch (event->key.key) {
            case SDLK_LSHIFT:
            case SDLK_RSHIFT:
                app->shiftPressed = down;
                break;
            case SDLK_LCTRL:
            case SDLK_RCTRL:
                app->ctrlPressed = down;
                break;
            case SDLK_LALT:
            case SDLK_RALT:
                app->altPressed = down;
                break;
            case SDLK_SPACE:
                app->spacePressed = down;
                break;
            default:
                break;
        }
    }

    // App-level events, handled whatever the current state is
    if (app->shouldQuit) {
        // The quit sequence has started
        return;
    }
    if ((event->type == SDL_EVENT_QUIT)
        || (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
            && event->window.windowID == SDL_GetWindowID(app->window))) {
        SDL_HideWindow(app->window);
        app->hidden = true;
        app->ShouldQuit();
        return;
    }
    if (event->type == SDL_EVENT_WINDOW_RESIZED) {
        app->Resize(event->window.data1, event->window.data2);
        return;
    }

    current->OnEvent(event);

    if (next != nullptr) {
        current = std::move(next);
    }
}

// ---------------------------------------------------------------------------
// IdleState

IdleState::IdleState(App* app) : IState(app) {}

std::string IdleState::Name() const {
    return "Idle";
}

void IdleState::OnEvent(const SDL_Event* event) {
    if (IsPointerDown(app, event) && app->canvas.selectedLayer != 0 && !app->canvas.strokeStarted) {
        app->stateManager.Switch(std::make_unique<StrokeState>(app, app->canvas.eraserMode));
        return;
    }

    if (event->type != SDL_EVENT_KEY_DOWN) {
        return;
    }

    if (event->key.key == SDLK_SPACE && !event->key.repeat) {
        app->stateManager.Switch(std::make_unique<NavigateState>(app));
        return;
    }
    if (IsKeyPressed(event, SDLK_S, SDL_KMOD_CTRL)) {
        app->Save();
        return;
    }
    if (IsKeyPressed(event, SDLK_F11, SDL_KMOD_NONE)) {
        app->Fullscreen(!app->fullscreen);
        return;
    }
    if (IsKeyPressed(event, SDLK_TAB, SDL_KMOD_NONE)) {
        app->hideUi = !app->hideUi;
        return;
    }
    if (IsKeyPressed(event, SDLK_Z, SDL_KMOD_CTRL)) {
        app->canvas.canvasCommands.Undo();
        return;
    }
    if (IsKeyPressed(event, SDLK_Z, SDL_KMOD_CTRL | SDL_KMOD_SHIFT)) {
        app->canvas.canvasCommands.Redo();
        return;
    }
    if (IsKeyPressed(event, SDLK_LEFT, SDL_KMOD_ALT)) {
        app->canvas.viewCommands.Undo();
        return;
    }
    if (IsKeyPressed(event, SDLK_RIGHT, SDL_KMOD_ALT)) {
        app->canvas.viewCommands.Redo();
        return;
    }
    if (IsKeyPressed(event, SDLK_B, SDL_KMOD_NONE)) {
        app->canvas.brushMode = true;
        app->canvas.eraserMode = false;
        return;
    }
    if (IsKeyPressed(event, SDLK_E, SDL_KMOD_NONE)) {
        app->canvas.brushMode = false;
        app->canvas.eraserMode = true;
        return;
    }
}

void IdleState::DrawUI() const {
    ImGui::Text("Tool: %s", app->canvas.eraserMode ? "Eraser" : "Brush");
}

// ---------------------------------------------------------------------------
// NavigateState

NavigateState::NavigateState(App* app) : IState(app) {
    // TODO: Save the previous view state so leaving the state can push a ViewportChangeCommand
}

std::string NavigateState::Name() const {
    return "Navigate";
}

void NavigateState::OnEvent(const SDL_Event* event) {
    if (event->type == SDL_EVENT_KEY_UP && event->key.key == SDLK_SPACE) {
        app->stateManager.Switch(std::make_unique<IdleState>(app));
        return;
    }

    if (IsPointerDown(app, event)) {
        startPos = app->cursorCurrentPos;
        dragging = true;
        return;
    }
    if (IsPointerUp(app, event)) {
        dragging = false;
        return;
    }

    if (IsKeyPressed(event, SDLK_R, SDL_KMOD_NONE)) {
        app->canvas.viewport.SetRotation(0.0f);
        app->canvas.viewport.SetZoom(glm::vec2(0.0f), glm::vec2(1.0f));
        return;
    }

    if (app->ctrlPressed && !app->shiftPressed) {
        mode = Mode::Zoom;
    } else if (app->shiftPressed && !app->ctrlPressed) {
        mode = Mode::Rotate;
    } else {
        mode = Mode::Pan;
    }

    if (IsPointerMove(app, event) && dragging) {
        const glm::vec2 center = glm::vec2(app->windowSize) / 2.0f;
        switch (mode) {
            case Mode::Pan:
                app->canvas.viewport.Translate(app->cursorDeltaPos);
                break;
            case Mode::Zoom:
                app->canvas.viewport.Zoom(startPos - center, glm::vec2(app->cursorDeltaPos.x / 1000.0f));
                break;
            case Mode::Rotate: {
                const auto previousAngle = std::atan2(app->cursorLastPos.x - center.x, app->cursorLastPos.y - center.y);
                const auto currentAngle =
                    std::atan2(app->cursorCurrentPos.x - center.x, app->cursorCurrentPos.y - center.y);
                app->canvas.viewport.Rotate(previousAngle - currentAngle);
                break;
            }
        }
    }
}

void NavigateState::DrawUI() const {
    ImGui::Text("Dragging: %s", dragging ? "true" : "false");
    switch (mode) {
        case Mode::Pan:
            ImGui::Text("Mode: Pan");
            break;
        case Mode::Rotate:
            ImGui::Text("Mode: Rotate");
            break;
        case Mode::Zoom:
            ImGui::Text("Mode: Zoom");
            break;
    }
}

// ---------------------------------------------------------------------------
// StrokeState

StrokeState::StrokeState(App* app, const bool erasing) : IState(app), erasing(erasing) {
    if (erasing) {
        app->canvas.StartEraserStroke(MakeStrokePoint(app, true));
    } else {
        app->canvas.StartBrushStroke(MakeStrokePoint(app, false));
    }
}

std::string StrokeState::Name() const {
    return erasing ? "Stroke (Eraser)" : "Stroke (Brush)";
}

void StrokeState::OnEvent(const SDL_Event* event) {
    if (IsPointerMove(app, event)) {
        if (erasing) {
            app->canvas.UpdateEraserStroke(MakeStrokePoint(app, true));
        } else {
            app->canvas.UpdateBrushStroke(MakeStrokePoint(app, false));
        }
        return;
    }

    if (IsPointerUp(app, event)) {
        if (erasing) {
            app->canvas.EndEraserStroke(MakeStrokePoint(app, true));
        } else {
            app->canvas.EndBrushStroke(MakeStrokePoint(app, false));
        }
        app->stateManager.Switch(std::make_unique<IdleState>(app));
    }
}

void StrokeState::DrawUI() const {
    ImGui::Text("Points: %zu", app->canvas.strokePoints.size());
}

} // namespace Midori
