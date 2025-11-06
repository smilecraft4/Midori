#include "midori/app.h"

#include "SDL3/SDL_events.h"
#include "SDL3/SDL_log.h"
#include "SDL3/SDL_video.h"
#include "glm/gtc/type_ptr.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "midori/renderer.h"
#include "tracy/Tracy.hpp"

App::App() : renderer_(this) {}

bool App::Init() {
  ZoneScoped;
  window_ = SDL_CreateWindow("Midori", window_width_, window_height_,
                             SDL_WINDOW_RESIZABLE);
  if (window_ == nullptr) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to create window: %s",
                    SDL_GetError());
    return false;
  }

  if (!renderer_.Init(window_)) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to Initialize renderer");
    return false;
  }

  return true;
}

bool App::Update() {
  FrameMark;
  ZoneScoped;
  /* Call some function on the ui, canvas, and other (not the renderer)*/

  // TODO: Move this somewhere else
  {
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    ImGui::Begin("Test");
    ImGui::Text("This is some useful stuff");
    ImGui::Text("Midori average %.3f ms/frame (%.1f FPS)",
                1000.0f / io.Framerate, io.Framerate);
    ImGui::ColorEdit4("Background Color", glm::value_ptr(bg_color_));
    ImGui::End();

    ImGui::Render();
  }

  if (!renderer_.Render(window_)) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to render");
    return false;
  }

  return true;
}

bool App::Resize(const int width, const int height) {
  ZoneScoped;
  window_width_ = width;
  window_height_ = height;

  renderer_.Resize(width, height);

  return true;
}

void App::Quit() {
  ZoneScoped;

  renderer_.Quit();
  SDL_DestroyWindow(window_);
}

bool App::ProcessSDLEvent(SDL_Event* event) {
  ImGui_ImplSDL3_ProcessEvent(event);
  const ImGuiIO& io = ImGui::GetIO();
  return io.WantCaptureMouse || io.WantCaptureKeyboard;
}