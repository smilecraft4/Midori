#include "midori/app.h"

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

namespace Midori {

App::App(int argc, char *argv[]) {
  args.resize(argc);
  for (size_t i = 0; i < args.size(); i++) {
    args[i] = std::string(argv[i]);
  }
}

bool App::Init() {
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

  gpu_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, "vulkan");
  if (gpu_device == nullptr) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to create gpu device: %s", SDL_GetError());
    return false;
  }

  if (!SDL_ClaimWindowForGPUDevice(gpu_device, window)) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION,
                    "Failed to create gpu device: %s", SDL_GetError());
    return false;
  }
  SDL_SetGPUSwapchainParameters(gpu_device, window,
                                SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                SDL_GPU_PRESENTMODE_VSYNC);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  (void)io;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  ImGui::StyleColorsLight();

  ImGuiStyle &style = ImGui::GetStyle();
  style.ScaleAllSizes(main_scale);
  style.FontScaleDpi = main_scale;

  // Setup Platform/Renderer backends
  ImGui_ImplSDL3_InitForSDLGPU(window);
  ImGui_ImplSDLGPU3_InitInfo init_info = {};
  init_info.Device = gpu_device;
  init_info.ColorTargetFormat =
      SDL_GetGPUSwapchainTextureFormat(gpu_device, window);
  init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
  init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
  init_info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
  ImGui_ImplSDLGPU3_Init(&init_info);

  return true;
}

bool App::Render() {
  if (window_size.x == 0 || window_size.y == 0) {
    return true;
  }

  ImGui_ImplSDLGPU3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();

  ImGui::Begin("Midori!");
  ImGui::ColorEdit4("Background Color", glm::value_ptr(bg_color));
  ImGui::End();

  ImGui::Render();

  SDL_GPUCommandBuffer *command_buffer =
      SDL_AcquireGPUCommandBuffer(gpu_device);
  if (command_buffer == nullptr) {
    SDL_LogError(SDL_LOG_CATEGORY_RENDER,
                 "Failed to acquire gpu command buffer: %s", SDL_GetError());
    return false;
  }

  SDL_GPUTexture *swapchain_texture;
  SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer, window,
                                        &swapchain_texture, nullptr, nullptr);
  if (swapchain_texture == nullptr) {
    SDL_LogError(SDL_LOG_CATEGORY_RENDER,
                 "Failed to acquire swapchain texture: %s", SDL_GetError());
    return false;
  }

  {
    SDL_GPUColorTargetInfo target_info = {
        .texture = swapchain_texture,
        .clear_color =
            SDL_FColor{bg_color.r, bg_color.g, bg_color.b, bg_color.a},
        .load_op = SDL_GPU_LOADOP_CLEAR,
        .store_op = SDL_GPU_STOREOP_STORE,
    };
    SDL_GPURenderPass *render_pass =
        SDL_BeginGPURenderPass(command_buffer, &target_info, 1, nullptr);
    SDL_EndGPURenderPass(render_pass);
  }

  {
    ImDrawData *draw_data = ImGui::GetDrawData();

    ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);
    SDL_GPUColorTargetInfo target_info = {
        .texture = swapchain_texture,
        .load_op = SDL_GPU_LOADOP_LOAD,
        .store_op = SDL_GPU_STOREOP_STORE,
    };
    SDL_GPURenderPass *render_pass =
        SDL_BeginGPURenderPass(command_buffer, &target_info, 1, nullptr);
    ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, render_pass);
    SDL_EndGPURenderPass(render_pass);
  }

  if (!SDL_SubmitGPUCommandBuffer(command_buffer)) {
    SDL_LogError(SDL_LOG_CATEGORY_RENDER,
                 "Failed to submit gpu command buffer: %s", SDL_GetError());
    return false;
  }

  return true;
} // namespace Midori

bool App::Resize(const int width, const int height) {
  window_size.x = width;
  window_size.y = height;

  return true;
}

void App::Quit() {
  SDL_WaitForGPUIdle(gpu_device);
  ImGui_ImplSDL3_Shutdown();
  ImGui_ImplSDLGPU3_Shutdown();
  ImGui::DestroyContext();

  SDL_ReleaseWindowFromGPUDevice(gpu_device, window);
  SDL_DestroyGPUDevice(gpu_device);
  gpu_device = nullptr;

  SDL_DestroyWindow(window);
  window = nullptr;
}

} // namespace Midori