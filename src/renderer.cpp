#include "midori/renderer.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_video.h>

#include <cassert>
#include <tracy/Tracy.hpp>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "midori/app.h"

Renderer::Renderer(App* app) : app_(app) { assert(app != nullptr); }

bool Renderer::Init(SDL_Window* window) {
  ZoneScoped;
  device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, "vulkan");
  if (device_ == nullptr) {
    SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to create SDL gpu device: %s",
                 SDL_GetError());
    return false;
  }

  if (!SDL_ClaimWindowForGPUDevice(device_, window)) {
    SDL_LogError(SDL_LOG_CATEGORY_RENDER,
                 "Failed to claim sdl window for gpu device: %s",
                 SDL_GetError());
    return false;
  }

  if (!SDL_SetGPUSwapchainParameters(device_, window,
                                     SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                     SDL_GPU_PRESENTMODE_VSYNC)) {
    SDL_LogError(SDL_LOG_CATEGORY_RENDER,
                 "Failed SDL_SetGPUSwapchainParameters: %s", SDL_GetError());
    return false;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.Fonts->AddFontFromFileTTF("/usr/share/fonts/OTF/CascadiaCode-Regular.otf",
                               16.0f);
  ImGui::StyleColorsLight();

  const SDL_GPUTextureFormat swapchain_texture_format =
      SDL_GetGPUSwapchainTextureFormat(device_, window);
  ImGui_ImplSDL3_InitForSDLGPU(window);
  ImGui_ImplSDLGPU3_InitInfo imgui_init_info = {
      .Device = device_,
      .ColorTargetFormat = swapchain_texture_format,
      .MSAASamples = SDL_GPU_SAMPLECOUNT_1,
      .SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
      .PresentMode = SDL_GPU_PRESENTMODE_VSYNC,
  };
  ImGui_ImplSDLGPU3_Init(&imgui_init_info);

  return true;
}

bool Renderer::Render(SDL_Window* window) {
  ImDrawData* draw_data = ImGui::GetDrawData();
  // TODO: Get this from a better place
  const bool is_minimized =
      (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);

  SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(device_);
  if (command_buffer == nullptr) {
    SDL_LogError(SDL_LOG_CATEGORY_RENDER,
                 "Failed to acquire gpu command buffer: %s", SDL_GetError());
    return false;
  }

  SDL_GPUTexture* swapchain_texture;
  if (!SDL_WaitAndAcquireGPUSwapchainTexture(
          command_buffer, window, &swapchain_texture, nullptr, nullptr)) {
    SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire GPU swapchain: %s",
                 SDL_GetError());
    return false;
  }

  if (!is_minimized) {
    ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);

    // Setup and start a render pass
    SDL_GPUColorTargetInfo target_info = {};
    target_info.texture = swapchain_texture;
    target_info.clear_color = SDL_FColor{app_->bg_color_.r, app_->bg_color_.g,
                                         app_->bg_color_.b, app_->bg_color_.a};
    target_info.load_op = SDL_GPU_LOADOP_CLEAR;
    target_info.store_op = SDL_GPU_STOREOP_STORE;
    target_info.mip_level = 0;
    target_info.layer_or_depth_plane = 0;
    target_info.cycle = false;
    SDL_GPURenderPass* render_pass =
        SDL_BeginGPURenderPass(command_buffer, &target_info, 1, nullptr);

    // Render ImGui
    ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, render_pass);

    SDL_EndGPURenderPass(render_pass);
  }

  if (!SDL_SubmitGPUCommandBuffer(command_buffer)) {
    SDL_LogError(SDL_LOG_CATEGORY_RENDER,
                 "Failed to submit GPU command buffer: %s", SDL_GetError());
    return false;
  }

  return true;
}

void Renderer::Quit() {
  ZoneScoped;
  SDL_WaitForGPUIdle(device_);

  ImGui_ImplSDL3_Shutdown();
  ImGui_ImplSDLGPU3_Shutdown();
  ImGui::DestroyContext();

  SDL_DestroyGPUDevice(device_);
}

bool Renderer::Resize(const int width, const int height) {
  ZoneScoped;
  viewport_.w = width;
  viewport_.h = height;
  /*Resize all layer texture and so on ... */

  return true;
}