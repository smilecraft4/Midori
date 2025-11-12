#include "midori/app.h"
#include "SDL3/SDL_log.h"

#include <format>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <ranges>
#include <tracy/Tracy.hpp>

namespace Midori {

App::App(int argc, char *argv[]) : renderer(this), canvas(this) {
  ZoneScoped;
  args.resize(argc);
  for (size_t i = 0; i < args.size(); i++) {
    args[i] = std::string(argv[i]);
  }
}

bool App::Init() {
  ZoneScoped;
  float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
  if (main_scale == 0.0F) {
    main_scale = 1.0F;
  }

  window = SDL_CreateWindow("Midori", window_size.x, window_size.y,
                            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN |
                                SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (window == nullptr) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to create window: %s",
                    SDL_GetError());
    return false;
  }
  SDL_SetWindowPosition(window, window_pos.x, window_pos.y);
  SDL_ShowWindow(window);

  if (!renderer.Init()) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to initialize renderer");
    return false;
  }

  if (!canvas.New()) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to create new canvas");
    return false;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  (void)io;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  ImGui::StyleColorsLight();

  ImGuiStyle &style = ImGui::GetStyle();
  style.ScaleAllSizes(main_scale);
  style.FontScaleDpi = main_scale;
  // TODO: load cascadia font

  ImGui_ImplSDL3_InitForSDLGPU(window);
  ImGui_ImplSDLGPU3_InitInfo init_info = {
      .Device = renderer.device,
      .ColorTargetFormat =
          SDL_GetGPUSwapchainTextureFormat(renderer.device, window),
      .MSAASamples = SDL_GPU_SAMPLECOUNT_1,
      .SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
      .PresentMode = SDL_GPU_PRESENTMODE_VSYNC,
  };
  ImGui_ImplSDLGPU3_Init(&init_info);

  return true;
}

bool App::Update() {
  FrameMark;
  if (window_size.x == 0 || window_size.y == 0) {
    return true;
  }

  canvas.Update();

  { // ImGui Stuff
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("Midori");
    for (auto &[layer, info] : std::ranges::reverse_view(canvas.layer_infos)) {
      ImGui::PushID(layer);
      std::string layer_name = std::format("{}", info.name);
      ImGui::Checkbox(layer_name.c_str(), &info.hidden);
      ImGui::PopID();
    }
    ImGui::End();

    ImGui::Render();
  }

  if (!renderer.Render()) {
    return false;
  }

  return true;
} // namespace Midori

bool App::Resize(const int width, const int height) {
  ZoneScoped;
  window_size.x = width;
  window_size.y = height;

  if (window_size.x > 0 || window_size.y > 0) {
    if (!renderer.Resize()) {
      return false;
    }
  }

  return true;
}

void App::Quit() {
  ZoneScoped;
  SDL_WaitForGPUIdle(renderer.device);
  ImGui_ImplSDL3_Shutdown();
  ImGui_ImplSDLGPU3_Shutdown();
  ImGui::DestroyContext();

  canvas.Quit();
  renderer.Quit();

  SDL_DestroyWindow(window);
  window = nullptr;
}

} // namespace Midori
