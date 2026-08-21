#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <tracy/Tracy.hpp>

#include "app.hpp"

static Uint32 SDLCALL Heartbeat(void* /*userdata*/, SDL_TimerID /*id*/, Uint32 interval) {
    SDL_Event event{};
    event.type = SDL_EVENT_USER;
    SDL_PushEvent(&event);
    return interval;
}

SDL_AppResult SDL_AppInit(void** appstate, int argc, char** argv) {
    SDL_SetMemoryFunctions(Midori::Malloc, Midori::Calloc, Midori::Realloc, Midori::Free);
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "waitevent");

    auto* app = new Midori::App(argc, argv);
    *appstate = app;

    if (!app->Init()) {
        return SDL_APP_FAILURE;
    }

    // SDL_AddTimer(250, Heartbeat, nullptr);

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    FrameMarkStart(nullptr);
    auto* app = static_cast<Midori::App*>(appstate);

    app->Update();

    if (app->CanQuit() && app->shouldQuit) {
        FrameMarkEnd(nullptr);
        return SDL_APP_SUCCESS;
    }

    FrameMarkEnd(nullptr);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    auto* app = static_cast<Midori::App*>(appstate);

    app->OnEvent(event);

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult /*result*/) {
    auto* app = static_cast<Midori::App*>(appstate);

    app->Quit();

    delete app;
}
