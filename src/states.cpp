#include "states.hpp"

#include "app.hpp"
#include "brushes.hpp"
#include "defines.hpp"

#include <backends/imgui_impl_sdl3.h>
#include <imgui.h>
#include <memory>
#include <utility>

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
} // namespace

IState::IState(App* app) : app(app) {}

StateManager::StateManager(App* app) : app(app), current(std::make_unique<IdleState>(app)) {}

void StateManager::Switch(std::unique_ptr<IState> state) {
    next = std::move(state);
}

void StateManager::OnEvent(const SDL_Event* event) {
    ImGui_ImplSDL3_ProcessEvent(event);

    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || io.WantCaptureMouse) {
        return;
    }

    if (event->type == SDL_EVENT_PEN_PROXIMITY_IN) {
        app->penInRange = true;
        app->penPressure = 0.0f;
    }
    if (event->type == SDL_EVENT_PEN_PROXIMITY_OUT) {
        app->penInRange = false;
        app->penPressure = 1.0f;
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

void IdleState::OnEvent(const SDL_Event* event) {
    if (IsPointerDown(app, event) && app->canvas.selectedLayer != LAYER_INVALID) {
        switch (app->canvas.currentTool) {
            case Tool::Paint: {
                app->stateManager.Switch(std::make_unique<PaintState>(app));
            } break;
            case Tool::Erase: {
                app->stateManager.Switch(std::make_unique<EraseState>(app));
            } break;
            default:
                SDL_assert(false && "Uknown tool");
                break;
        }

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
        app->canvas.currentTool = Tool::Paint;
        return;
    }
    if (IsKeyPressed(event, SDLK_E, SDL_KMOD_NONE)) {
        app->canvas.currentTool = Tool::Erase;
        return;
    }
}

void IdleState::DrawUI() const {
    ImGui::Text("IdleState");
    ImGui::Text("Tool: %s", TOOL_NAME[std::to_underlying(app->canvas.currentTool)]);
}

// ---------------------------------------------------------------------------
// NavigateState

NavigateState::NavigateState(App* app) : IState(app) {
    // TODO: Save the previous view state so leaving the state can push a ViewportChangeCommand
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
            case Mode::Zoom: {
                app->canvas.viewport.Zoom(startPos - center, glm::vec2(app->cursorDeltaPos.x / 256.0f));
                break;
            }
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
    ImGui::Text("NavigateState");
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

Canvas::StrokePoint MakeStrokePoint(const App* app, BrushSettings settings) {
    Canvas::StrokePoint point{};
    point.position = app->canvas.viewport.ScreenToCanvas(app->cursorCurrentPos);
    point.color = app->canvas.paintBrush.color;
    point.color.a = std::lerp(settings.minPressureOpacity, settings.maxPressureOpacity, app->penPressure);
    point.radius = std::lerp(settings.minPressureRadius, settings.maxPressureRadius, app->penPressure);
    point.hardness = std::lerp(settings.minPressureHardness, settings.maxPressureHardness, app->penPressure);
    point.flow = std::lerp(settings.minPressureFlow, settings.maxPressureFlow, app->penPressure);
    return point;
}

PaintState::PaintState(App* app) : IState(app) {
    const auto point = MakeStrokePoint(app, app->canvas.paintBrush);
    app->canvas.StartPaintStroke(point);
}

void PaintState::OnEvent(const SDL_Event* event) {
    if (IsPointerMove(app, event)) {
        const auto point = MakeStrokePoint(app, app->canvas.paintBrush);
        app->canvas.UpdatePaintStroke(point);
    }

    if (IsPointerUp(app, event)) {
        const auto point = MakeStrokePoint(app, app->canvas.paintBrush);
        app->canvas.EndPaintStroke(point);
        app->stateManager.Switch(std::make_unique<IdleState>(app));
    }
}

void PaintState::DrawUI() const {
    ImGui::Text("PaintState");
    ImGui::Text("Points: %zu", app->canvas.strokePoints.size());
}

EraseState::EraseState(App* app) : IState(app) {
    const auto point = MakeStrokePoint(app, app->canvas.eraseBrush);
    app->canvas.StartEraseStroke(point);
}

void EraseState::OnEvent(const SDL_Event* event) {
    if (IsPointerMove(app, event)) {
        const auto point = MakeStrokePoint(app, app->canvas.eraseBrush);
        app->canvas.UpdateEraseStroke(point);
    }

    if (IsPointerUp(app, event)) {
        const auto point = MakeStrokePoint(app, app->canvas.eraseBrush);
        app->canvas.EndEraseStroke(point);
        app->stateManager.Switch(std::make_unique<IdleState>(app));
    }
}

void EraseState::DrawUI() const {
    ImGui::Text("EraseState");
    ImGui::Text("Points: %zu", app->canvas.strokePoints.size());
}
} // namespace Midori
