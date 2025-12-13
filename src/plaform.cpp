#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <cstdlib>

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <tracy/Tracy.hpp>

#include "app.h"
#include "platform.h"
#include "renderer.h"

std::vector<std::string> gArgs;
App gApp;

void* Platform::Malloc(size_t size) {
    auto* mem = std::malloc(size);
    TracyAlloc(mem, size);
    return mem;
};

void* Platform::Calloc(size_t nmemb, size_t size) {
    auto* mem = std::calloc(nmemb, size);
    TracyAlloc(mem, size);
    return mem;
};

void* Platform::Realloc(void* mem, size_t size) {
    TracyFree(mem);
    auto* new_mem = std::realloc(mem, size);
    TracyAlloc(new_mem, size);
    return new_mem;
};

void Platform::Free(void* mem) {
    TracyFree(mem);
    std::free(mem);
}

void* operator new(size_t Size) {
    auto* ptr = Platform::Malloc(Size);
    return ptr;
}
void operator delete(void* ptr) noexcept { Platform::Free(ptr); }

SDL_Window* gWindow;
int32_t gWindowWidth;
int32_t gWindowHeight;
Uint64 gLastTicks;
float gDeltaTime;

SDL_AppResult SDL_AppInit(void** appstate, int argc, char** argv) {
    ZoneScoped;
    gLastTicks = SDL_GetTicksNS();
    gDeltaTime = 0.0f;

    // SDL_SetMemoryFunctions(Platform::Malloc, Platform::Calloc, Platform::Realloc, Platform::Free);

    float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (main_scale == 0.0F) {
        main_scale = 1.0F;
    }

    gWindowWidth = 1920;
    gWindowHeight = 1080;

    {
        ZoneScopedN("SDL_CreateWindow");
        gWindow = SDL_CreateWindow("Midori", gWindowWidth, gWindowHeight,
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (gWindow == nullptr) {
            SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to create window: %s", SDL_GetError());
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Midori", "Failed to create window", nullptr);
            return SDL_APP_FAILURE;
        }
    }
    {
        ZoneScopedN("ImGui::CreateContext");
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        ImGui::StyleColorsLight();

        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(main_scale);
        style.FontScaleDpi = main_scale;
    }

    // Init renderer
    if (const auto result = Renderer::InitSDL3(gWindow); result != Renderer::Result::Success) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize renderer");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Midori", "Failed to initialize Renderer", gWindow);
        return SDL_APP_FAILURE;
    }

    Renderer::Resize(gWindowWidth, gWindowHeight);

    if (const auto result = gApp.Init(argc, argv); result != App::Result::Success) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize midori");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Midori", "Failed to initialize midori", gWindow);
        return SDL_APP_FAILURE;
    }

    {
        ZoneScopedN("Show window");
        if (!SDL_ShowWindow(gWindow)) {
            SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to show window");
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Midori", "Failed to show window", gWindow);
            return SDL_APP_FAILURE;
        }
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    FrameMark;
    Uint64 currentTicks = SDL_GetTicksNS();
    Uint64 diffTicks = currentTicks - gLastTicks;
    gLastTicks = currentTicks;
    gDeltaTime = (float)diffTicks / 1'000'000'000.0f;

    if (gWindowWidth > 0 && gWindowHeight > 0) {
        Platform::WindowRender();
    }

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    if (event->type == SDL_EVENT_QUIT) {
        return SDL_APP_SUCCESS;
    }
    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event->window.windowID == SDL_GetWindowID(gWindow)) {
        return SDL_APP_SUCCESS;
    }

    if (event->type == SDL_EVENT_WINDOW_RESIZED) {
        gWindowWidth = event->window.data1;
        gWindowHeight = event->window.data2;
        if (event->window.data1 > 0 && event->window.data2 > 0) {
            if (const auto result = Renderer::Resize(event->window.data1, event->window.data2);
                result != Renderer::Result::Success) {
                SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to resize renderer");
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Midori", "Failed to resize renderer", gWindow);
                return SDL_APP_FAILURE;
            }
        }
        gApp.OnWindowResize(event->window.data1, event->window.data2);

        Platform::WindowRender();
    }

    ImGui_ImplSDL3_ProcessEvent(event);

    ImGuiIO& io = ImGui::GetIO();

    if (!io.WantCaptureKeyboard && !io.WantCaptureMouse) {
        // Keyboard
        if (event->type == SDL_EVENT_KEY_DOWN) {
            gApp.OnKeyPress(event->key.key, event->key.mod);
        }
        if (event->type == SDL_EVENT_KEY_UP) {
            gApp.OnKeyRelease(event->key.key, event->key.mod);
        }

        if (event->type == SDL_EVENT_MOUSE_MOTION) {
            gApp.OnCursorMove(event->motion.x, event->motion.y);
        }
        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            gApp.OnButtonPress(event->button.button);
        }
        if (event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
            gApp.OnButtonRelease(event->button.button);
        }

        // Stylus
        if (event->type == SDL_EVENT_PEN_PROXIMITY_IN) {
            gApp.OnCursorEnter();
        }
        if (event->type == SDL_EVENT_PEN_PROXIMITY_OUT) {
            gApp.OnCursorExit();
        }
        if (event->type == SDL_EVENT_PEN_MOTION) {
            gApp.OnCursorMove(event->pmotion.x, event->pmotion.y);
        }
        if (event->type == SDL_EVENT_PEN_AXIS) {
            if (event->paxis.axis == SDL_PEN_AXIS_PRESSURE) {
                gApp.OnCursorPressure(event->paxis.value);
            }
        }
        // if (event->type == SDL_EVENT_PEN_DOWN) {
        //     gApp.OnButtonPress(App::Button::Left);
        // }
        // if (event->type == SDL_EVENT_PEN_UP) {
        //     gApp.OnButtonRelease(App::Button::Left);
        // }
    }

    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result) {
    if (const auto res = gApp.Terminate(); res != App::Result::Success) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Found error will closing App");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Midori", "Found error will closing App", gWindow);
    }

    ImGui_ImplSDL3_Shutdown();
    Renderer::TerminateSDL3();
    ImGui::DestroyContext();
    SDL_DestroyWindow(gWindow);
}

Platform::Result Platform::SetWindowPos(int32_t x, int32_t y) {
    SDL_assert(gWindow);
    if (!SDL_SetWindowPosition(gWindow, x, y)) {
        return Result::Unknown;
    }
    return Result::Success;
}

Platform::Result Platform::SetWindowSize(int32_t width, int32_t height) {
    SDL_assert(gWindow);
    if (!SDL_SetWindowSize(gWindow, width, height)) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Failed to set window size: %s", SDL_GetError());
        return Result::Unknown;
    }
    return Result::Success;
}

Platform::Result Platform::SetWindowTitle(const std::string& title) {
    SDL_assert(gWindow);
    if (!SDL_SetWindowTitle(gWindow, title.c_str())) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Failed to set window title: %s", SDL_GetError());
        return Result::Unknown;
    }
    return Result::Success;
}

Platform::Result Platform::SetWindowFullscreened(bool fullscreen) {
    SDL_assert(gWindow);
    if (!SDL_SetWindowFullscreen(gWindow, fullscreen)) {
        if (fullscreen) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Failed to set window fullscreen: %s", SDL_GetError());
        } else {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Failed to unset window fullscreen: %s", SDL_GetError());
        }
        return Result::Unknown;
    }
    if (!SDL_SyncWindow(gWindow)) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Failed to sync window: %s", SDL_GetError());
        return Result::Unknown;
    }
    return Result::Success;
}

Platform::Result Platform::SetWindowMaximized(bool maximize) {
    SDL_assert(gWindow);
    if (maximize) {
        if (!SDL_MaximizeWindow(gWindow)) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Failed to maximize window: %s", SDL_GetError());
            return Result::Unknown;
        }
    } else {
        if (!SDL_RestoreWindow(gWindow)) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "Failed to restore window: %s", SDL_GetError());
            return Result::Unknown;
        }
    }
    return Result::Success;
}

Platform::Result Platform::WindowRender() {
    ImGui_ImplSDL3_NewFrame();
    Renderer::NewFrame();

    if (const auto result = gApp.OnWindowRender(gDeltaTime); result != App::Result::Success) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to render frame");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Midori", "Failed to render frame", gWindow);
        return Result::Unknown;
    }

    if (const auto result = Renderer::Render(gWindow); result != Renderer::Result::Success) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to render frame");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Midori", "Failed to render frame", gWindow);
        return Result::Unknown;
    }

    return Result::Success;
}

/*
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui_impl_sdl3.h>

#include "midori/app.h"
#include "midori/memory.h"

// Sort everything into function only from app
// App::Init(const std::vector<std::string>>& args);
// Public
// App::App(std::span<const std:string_view> args)
// App::OnChar(App::Key)
// App::OnKeyPress(App::Key)
// App::OnKeyRelease(App::Key)
// App::OnCursorMove(float x, float y)
// App::OnButtonPress(App::Button)
// App::OnButtonRelease(App::Button)
// App::OnRender(float deltaTime)
// App::OnUpdate(float deltaTime)
// App::~App()
//
// Private
// App::New();
// App::Open();
// App::Save();
// App::SaveAs();

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

  if (!app->Update()) {
    return SDL_APP_FAILURE;
  }

  return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
  auto *app = static_cast<Midori::App *>(appstate);

  if (event->type == SDL_EVENT_QUIT) {
    return SDL_APP_SUCCESS;
  }
  if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
      event->window.windowID == SDL_GetWindowID(app->window)) {
    return SDL_APP_SUCCESS;
  }

  if (event->type == SDL_EVENT_WINDOW_RESIZED) {
    app->Resize(event->window.data1, event->window.data2);
  }

  ImGui_ImplSDL3_ProcessEvent(event);

  ImGuiIO &io = ImGui::GetIO();

  if (!io.WantCaptureKeyboard && !io.WantCaptureMouse) {
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
*/