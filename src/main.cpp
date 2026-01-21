#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <backends/imgui_impl_sdl3.h>

#include "app.h"
#include "layers.h"
#include "memory.h"

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
    SDL_SetMemoryFunctions(Midori::Malloc, Midori::Calloc, Midori::Realloc, Midori::Free);
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "waitevent");

    auto *app = new Midori::App(argc, argv);
    *appstate = app;

    if (!app->Init()) {
        return SDL_APP_FAILURE;
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    auto *app = static_cast<Midori::App *>(appstate);

    app->Update();

    if (app->CanQuit() && app->should_quit) {
        return SDL_APP_SUCCESS;
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    auto *app = static_cast<Midori::App *>(appstate);

    if(app->should_quit) {
        // prevents any new event from ocuring once the app is closing
        return SDL_APP_CONTINUE;
    }

    if ((event->type == SDL_EVENT_QUIT) || (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event->window.windowID == SDL_GetWindowID(app->window))) {
        SDL_HideWindow(app->window);
        SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "15"); // Keep checking every 15Hz to see if everything is finished
        app->hidden = true;
        app->ShouldQuit();
    }


    if (event->type == SDL_EVENT_WINDOW_RESIZED) {
        app->Resize(event->window.data1, event->window.data2);
    }

    ImGui_ImplSDL3_ProcessEvent(event);

    ImGuiIO &io = ImGui::GetIO();

    if (io.WantCaptureMouse) {
        if (!SDL_ShowCursor()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to show cursor: %s", SDL_GetError());
        }
    } else if (!io.WantCaptureMouse) {
        if (!SDL_HideCursor()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to hide cursor: %s", SDL_GetError());
        }
    }

    app->ui_focus = io.WantCaptureMouse;  // || io.WantCaptureKeyboard;

    if (!app->ui_focus) {
        // Keyboard
        if (event->type == SDL_EVENT_KEY_DOWN) {
            app->KeyPress(event->key.key, event->key.mod);
        }
        if (event->type == SDL_EVENT_KEY_UP) {
            app->KeyRelease(event->key.key, event->key.mod);
        }

        // Mouse
        if (!app->pen_in_range) {
            if (event->type == SDL_EVENT_MOUSE_MOTION) {
                app->CursorMove(glm::vec2(event->motion.x, event->motion.y));
            }
            if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                app->CursorPress(event->button.button);
            }
            if (event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
                app->CursorRelease(event->button.button);
            }
        }

        // Stylus
        if (event->type == SDL_EVENT_PEN_PROXIMITY_IN) {
            app->pen_in_range = true;
        }
        if (event->type == SDL_EVENT_PEN_PROXIMITY_OUT) {
            app->pen_in_range = false;
        }
        if (app->pen_in_range) {
            if (event->type == SDL_EVENT_PEN_MOTION) {
                app->CursorMove(glm::vec2(event->pmotion.x, event->pmotion.y));
            }
            if (event->type == SDL_EVENT_PEN_AXIS) {
                if (event->paxis.axis == SDL_PEN_AXIS_PRESSURE) {
                    app->pen_pressure = event->paxis.value;
                }
            }
            if (event->type == SDL_EVENT_PEN_DOWN) {
                app->CursorPress(SDL_BUTTON_LEFT);
            }
            if (event->type == SDL_EVENT_PEN_UP) {
                app->CursorRelease(SDL_BUTTON_LEFT);
            }
        }
    }

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    auto *app = static_cast<Midori::App *>(appstate);

    app->Quit();
    delete app;
}
