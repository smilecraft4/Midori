#include "SDL3/SDL_events.h"
#include "SDL3/SDL_init.h"
#include "SDL3/SDL_log.h"
#define SDL_MAIN_USE_CALLBACKS 1
#include "SDL3/SDL_main.h"
#include "midori/app.h"

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
  auto *app = new App();
  *appstate = app;

  if (!app->Init()) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to Init application");
    return SDL_APP_FAILURE;
  }

  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
  auto *app = static_cast<App *>(appstate);

  if (!app->Update()) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to Render application");
    return SDL_APP_FAILURE;
  }

  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
  auto *app = static_cast<App *>(appstate);

  if (event->type == SDL_EVENT_QUIT) {
    return SDL_APP_SUCCESS;
  }
  if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
      event->window.windowID == SDL_GetWindowID(app->window_)) {
    return SDL_APP_SUCCESS;
  }

  if (App::ProcessSDLEvent(event)) {
    return SDL_APP_CONTINUE;
  }

  return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  auto *app = static_cast<App *>(appstate);

  app->Quit();
  delete app;
}