#include "states.h"

#include "app.h"
#include <SDL3/SDL_assert.h>
#include <backends/imgui_impl_sdl3.h>
#include <memory>

namespace Midori {

IState::IState(App* app) : app(app) {}

StateManager::StateManager(App* app) : app(app) {}

StateManager::~StateManager() {
    while (!states.empty()) {
        Pop();
    }
}

void StateManager::Switch(std::unique_ptr<IState> state) {
    Pop();
    Push(std::move(state));
}

void StateManager::Push(std::unique_ptr<IState> state) {
    states.push_back(std::move(state));
}

void StateManager::Pop() {
    states.pop_back();
}

void StateManager::OnEvent(const SDL_Event* event) {
    ImGui_ImplSDL3_ProcessEvent(event);

    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard) {
        if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) {
            return;
        }
    }
    if (io.WantCaptureMouse) {
        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_UP
            || event->type == SDL_EVENT_MOUSE_WHEEL || event->type == SDL_EVENT_MOUSE_MOTION) {
            return;
        }
    }

    if (event->type == SDL_EVENT_MOUSE_MOTION) {
        const glm::vec2 newPos{event->motion.x, event->motion.y};
        app->cursorLastPos = app->cursorCurrentPos;
        app->cursorCurrentPos.x = event->motion.x;
        app->cursorCurrentPos.y = event->motion.y;
        app->cursorDeltaPos = app->cursorCurrentPos - app->cursorLastPos;
    }

    // TODO Update inputManager current keybind

    for (int i = static_cast<int>(states.size()) - 1; i >= 0; i--) {
        // TODO See if the inputManager has a matching action for the current scoped state

        if (!states[i]->OnEvent(event)) {
            break;
        }
    }
}

AppState::AppState(App* app) : IState(app) {}

AppState::~AppState() {}

std::string AppState::Name() const {
    return "AppState";
}

bool AppState::OnEvent(const SDL_Event* event) {
    if (app->shouldQuit) {
        // The quit senquence as started
        return true;
    }

    if ((event->type == SDL_EVENT_QUIT)
        || (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
            && event->window.windowID == SDL_GetWindowID(app->window))) {
        SDL_HideWindow(app->window);
        app->hidden = true;
        app->ShouldQuit();
        return true;
    }

    if (event->type == SDL_EVENT_WINDOW_RESIZED) {
        app->Resize(event->window.data1, event->window.data2);
    }

    if (event->type == SDL_EVENT_KEY_DOWN) {
        constexpr SDL_Keymod MODS_IGNORED = SDL_KMOD_NUM | SDL_KMOD_CAPS | SDL_KMOD_SCROLL;

        if (event->key.key == SDLK_S && (event->key.mod & ~MODS_IGNORED) == SDL_KMOD_CTRL) {
            app->Save();
        }
        if (event->key.key == SDLK_F11 && (event->key.mod & ~MODS_IGNORED) == SDL_KMOD_NONE) {
            app->Fullscreen(!app->fullscreen); // TODO replace by the input manager
            return true;
        }
        if (event->key.key == SDLK_TAB && (event->key.mod & ~MODS_IGNORED) == SDL_KMOD_NONE) {
            app->hideUi = !app->hideUi;
            return true;
        }
        if (event->key.key == SDLK_Z && (event->key.mod & ~MODS_IGNORED) == SDL_KMOD_CTRL) {
            app->canvas.canvasCommands.Redo(); // TODO replace by the input manager
            return true;
        }
        if (event->key.key == SDLK_Z && (event->key.mod & ~MODS_IGNORED) == (SDL_KMOD_CTRL | SDL_KMOD_SHIFT)) {
            app->canvas.canvasCommands.Undo(); // TODO replace by the input manager
            return true;
        }
        if (event->key.key == SDLK_LEFT && (event->key.mod & ~MODS_IGNORED) == SDL_KMOD_ALT) {
            app->canvas.viewCommands.Undo();
            return true;
        }
        if (event->key.key == SDLK_RIGHT && (event->key.mod & ~MODS_IGNORED) == SDL_KMOD_ALT) {
            app->canvas.viewCommands.Redo();
            return true;
        }
        if (event->key.key == SDLK_SPACE && !event->key.repeat) {
            app->stateManager.Push(std::make_unique<NavigateState>(app));
        }
    }

    if (event->type == SDL_EVENT_KEY_UP) {}

    return false;
}

void AppState::DrawUI() const {
    ImGui::Text("App State ¯\\_(ツ)_/¯");
}

NavigateState::NavigateState(App* app) : IState(app) {
    // Save the previous view state for later comparaison
}

NavigateState::~NavigateState() {
    // If there was change with the previous view state add the changes to the view history
}

std::string NavigateState::Name() const {
    return "NavigateState";
}

bool NavigateState::OnEvent(const SDL_Event* event) {
    bool handled = false;
    constexpr SDL_Keymod MODS_IGNORED = SDL_KMOD_NUM | SDL_KMOD_CAPS | SDL_KMOD_SCROLL;

    // TODO: Transform everything to use InputManager instead

    // Examples of keybinds strategy for navigation keybinds
    // (Press) is the default state so "F" == "F (Press)"
    // version A
    // - NavigateTool: "Space (Hold)"
    // - NavigateTool.Pan: "" ? How to set the default entry state
    // - NavigateTool.Zoom: "Shift (Hold)"
    // - NavigateTool.Rotate: "Ctrl (Hold)"
    // - NavigateTool.FlipHorizontal: "F+H"
    // - NavigateTool.FlipVertical: "F+V"
    // - NavigateTool.RestoreFlipHorizontal: "Alt+F+H"
    // - NavigateTool.RestoreFlipVertical: "Alt+F+V"
    //
    // version B
    // - EnableNavigateTool: "Space (Press)"
    // - DisableNavigateTool: "Space (Release)"
    // - NavigateTool.EnableZoom: "Shift (Press)"
    // - NavigateTool.DisableZoom: "Shift (Release)"
    // - NavigateTool.EnableRotate: "Ctrl (Press)"
    // - NavigateTool.DisableRotate: "Ctrl (Release)"
    // - NavigateTool.FlipHorizontal: "F+H"
    // - NavigateTool.FlipVertical: "F+V"
    // - NavigateTool.RestoreFlipHorizontal: "Alt+F+H"
    // - NavigateTool.RestoreFlipVertical: "Alt+F+V"

    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (event->button.button == 1) {
            startPos = app->cursorCurrentPos;
            clicking = true;
            handled = true;
        }
    }

    if (event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (event->button.button == 1) {
            clicking = false;
            handled = true;
        }
    }

    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat) {
        if (event->key.key == SDLK_LSHIFT || event->key.key == SDLK_RSHIFT) {
            shift = true;
            handled = true;
        }
        if (event->key.key == SDLK_LCTRL || event->key.key == SDLK_RCTRL) {
            ctrl = true;
            handled = true;
        }
        if (event->key.key == SDLK_R) {
            app->canvas.viewport.SetRotation(0.0f);
            app->canvas.viewport.SetTranslation(glm::vec2(0.0f, 0.0f));
            app->canvas.viewport.SetZoom(glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 1.0f));
        }
    }

    if (event->type == SDL_EVENT_KEY_UP) {
        if (event->key.key == SDLK_LSHIFT || event->key.key == SDLK_RSHIFT) {
            shift = false;
            handled = true;
        }
        if (event->key.key == SDLK_LCTRL || event->key.key == SDLK_RCTRL) {
            ctrl = false;
            handled = true;
        }
        if (event->key.key == SDLK_SPACE) {
            // If there was change add this operation to the view history
            app->stateManager.Pop();
        }
    }

    if (ctrl && !shift) {
        mode = Mode::Zoom;
        handled = true;
    } else if (shift && !ctrl) {
        mode = Mode::Rotate;
        handled = true;
    } else if (mode != Mode::Pan) {
        mode = Mode::Pan;
        handled = true;
    }

    if (event->type == SDL_EVENT_MOUSE_MOTION && clicking) {
        const glm::vec2 cursorPos = glm::vec2(event->motion.x, event->motion.y);
        const glm::vec2 cursorRel = glm::vec2(event->motion.xrel, event->motion.yrel);

        if (mode == Mode::Rotate) {
            const auto startAngle = std::atan2(startPos.x - (static_cast<float>(app->windowSize.x) / 2.0f),
                                               startPos.y - (static_cast<float>(app->windowSize.y) / 2.0f));
            const auto previousAngle =
                std::atan2(app->cursorLastPos.x - (static_cast<float>(app->windowSize.x) / 2.0f),
                           app->cursorLastPos.y - (static_cast<float>(app->windowSize.y) / 2.0f));
            const auto currentAngle = std::atan2(cursorPos.x - (static_cast<float>(app->windowSize.x) / 2.0f),
                                                 cursorPos.y - (static_cast<float>(app->windowSize.y) / 2.0f));
            const auto angleDelta = (previousAngle - startAngle) - (currentAngle - startAngle);
            app->canvas.viewport.Rotate(angleDelta);
            handled = true;
        } else if (mode == Mode::Zoom) {
            app->canvas.viewport.Zoom(app->canvas.viewCursorStart, glm::vec2(app->cursorDeltaPos.x / 1000.0f));
            handled = true;
        } else if (mode == Mode::Pan) {
            app->canvas.viewport.Translate(app->cursorDeltaPos);
            handled = true;
        }
    }

    if (event->type == SDL_EVENT_PEN_MOTION) {}

    return handled;
}

void NavigateState::DrawUI() const {
    ImGui::Text("Navigate State");
    ImGui::Text("Clicking: %s", clicking ? "true" : "false");
    ImGui::Text("Space: %s", space ? "true" : "false");
    ImGui::Text("Shift: %s", shift ? "true" : "false");
    ImGui::Text("Alt: %s", ctrl ? "true" : "false");
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

} // namespace Midori
