#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "midori/app.h"
#include "midori/memory.h"

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
  SDL_SetMemoryFunctions(Midori::Malloc, Midori::Calloc, Midori::Realloc,
                         Midori::Free);

  auto *app = new Midori::App(argc, argv);
  *appstate = app;

  if (!app->Init()) {
    return SDL_APP_FAILURE;
  }

  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
  auto *app = static_cast<Midori::App *>(appstate);

  if (!app->Render()) {
    return SDL_APP_FAILURE;
  }

  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
  auto *app = static_cast<Midori::App *>(appstate);

  if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
      event->type == SDL_EVENT_QUIT) {
    return SDL_APP_SUCCESS;
  }

  return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
  auto *app = static_cast<Midori::App *>(appstate);

  app->Quit();
  delete app;
}
