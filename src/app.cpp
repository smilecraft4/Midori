#include "midori/app.h"

namespace Midori {

constexpr int WINDOW_DEFAULT_WIDTH = 1280;
constexpr int WINDOW_DEFAULT_HEIGHT = 720;

App::App(int argc, char *argv[]) {
  args.Reserve(argc);
  for (size_t i = 0; i < argc; i++) {
    args.Push(std::string(argv[i]));
  }
}

bool App::Init() {
  window = SDL_CreateWindow("Midori", WINDOW_DEFAULT_WIDTH,
                            WINDOW_DEFAULT_HEIGHT, SDL_WINDOW_RESIZABLE);
  if (window == nullptr) {
    SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to create window: %s",
                    SDL_GetError());
    return false;
  }

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

  return true;
}

bool App::Render() {
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

  SDL_GPUColorTargetInfo color_target_infos[1] = {{
      .texture = swapchain_texture,
      .clear_color = SDL_FColor{bg_color.r, bg_color.g, bg_color.b, bg_color.a},
      .load_op = SDL_GPU_LOADOP_CLEAR,
      .store_op = SDL_GPU_STOREOP_STORE,
  }};

  SDL_GPURenderPass *render_pass =
      SDL_BeginGPURenderPass(command_buffer, color_target_infos, 1, nullptr);

  SDL_EndGPURenderPass(render_pass);

  if (!SDL_SubmitGPUCommandBuffer(command_buffer)) {
    SDL_LogError(SDL_LOG_CATEGORY_RENDER,
                 "Failed to submit gpu command buffer: %s", SDL_GetError());
    return false;
  }

  return true;
}

void App::Quit() {
  SDL_DestroyGPUDevice(gpu_device);
  gpu_device = nullptr;

  SDL_DestroyWindow(window);
  window = nullptr;
}

} // namespace Midori