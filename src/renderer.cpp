#include "renderer.hpp"

#include "app.hpp"
#include "base.hpp"
#include "canvas.hpp"
#include "defines.hpp"
#include "errors.hpp"
#include "states.hpp"
#include "tiles.hpp"

#include "../deps/qoi.h"
#include <backends/imgui_impl_sdlgpu3.h>
#include <expected>
#include <glm/gtc/matrix_transform.hpp>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_assert.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_log.h>
#include <mutex>
#include <tracy/Tracy.hpp>
#include <imgui.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>

// #if defined(MIDORI_WINDOWS)
// #include "shaders/dxil/erase.cs.h"
// #include "shaders/dxil/layer.ps.h"
// #include "shaders/dxil/layer.vs.h"
// #include "shaders/dxil/merge.cs.h"
// #include "shaders/dxil/paint.cs.h"
// #include "shaders/dxil/tile.ps.h"
// #include "shaders/dxil/tile.vs.h"
// #elif defined(MIDORI_LINUX)
#include "shaders/spirv/erase.comp.h"
#include "shaders/spirv/layer.frag.h"
#include "shaders/spirv/layer.vert.h"
#include "shaders/spirv/merge.comp.h"
#include "shaders/spirv/paint.comp.h"
#include "shaders/spirv/tile.frag.h"
#include "shaders/spirv/tile.vert.h"
// #endif

namespace Midori {

std::expected<TileStageBufferSlot, Error> Renderer::ReserveTileUploadSlot(TileTransfer::ID id) {
    ZoneScoped;
    for (uint8_t buf = 0; buf < static_cast<uint8_t>(std::size(tileUploadStagingBuffers)); buf++) {
        std::unique_lock lock(tileUploadStagingBuffers[buf].mutex);
        if (tileUploadStagingBuffers[buf].state == StagingBufferState::None) {
            if (!tileUploadStagingBuffers[buf].used.all()) {
                for (uint8_t slot = 0; slot < static_cast<uint8_t>(tileUploadStagingBuffers[buf].used.size()); slot++) {
                    if (!tileUploadStagingBuffers[buf].used[slot]) {
                        SDL_assert(!tileUploadStagingBuffers[buf].commited[slot]
                                   && "Reserving an upload slot that is still marked committed");
                        tileUploadStagingBuffers[buf].used[slot] = true;
                        tileUploadStagingBuffers[buf].ids[slot] = id;

                        if (tileUploadStagingBuffers[buf].used.all()) {
                            tileUploadStagingBuffers[buf].state = StagingBufferState::Completed;
                        }

                        SDL_assert(tileUploadStagingBuffers[buf].used.all()
                                       == (tileUploadStagingBuffers[buf].state == StagingBufferState::Completed)
                                   && "Upload buffer state does not match its slot occupancy");

                        return TileStageBufferSlot{
                            .buf = buf,
                            .slot = slot,
                        };
                    }
                }
            }
        }
    }

    return std::unexpected(Error::OutOfCapacity);
}

std::expected<TileStageBufferSlot, Error> Renderer::GetTileUploadSlot(TileTransfer::ID id) {
    ZoneScoped;
    for (uint8_t buf = 0; buf < static_cast<uint8_t>(std::size(tileUploadStagingBuffers)); buf++) {
        std::unique_lock lock(tileUploadStagingBuffers[buf].mutex);
        for (uint8_t slot = 0; slot < static_cast<uint8_t>(tileUploadStagingBuffers[buf].used.size()); slot++) {
            if (tileUploadStagingBuffers[buf].used[slot] && tileUploadStagingBuffers[buf].ids[slot] == id) {
                return TileStageBufferSlot{
                    .buf = buf,
                    .slot = slot,
                };
            }
        }
    }
    return std::unexpected(Error::Unknown);
}

void* Renderer::GetTileUploadSlotData(TileStageBufferSlot slot) {
    ZoneScoped;
    SDL_assert(slot.buf < std::size(tileUploadStagingBuffers) && "Upload slot buffer index out of range");
    SDL_assert(slot.slot < TILE_UPLOAD_BUFFER_CAPACITY && "Upload slot index out of range");

    std::unique_lock lock(tileUploadStagingBuffers[slot.buf].mutex);
    SDL_assert(tileUploadStagingBuffers[slot.buf].used[slot.slot] && "Upload slot was not reserved");
    SDL_assert(!tileUploadStagingBuffers[slot.buf].commited[slot.slot] && "Upload slot data written after commit");
    SDL_assert((tileUploadStagingBuffers[slot.buf].state == StagingBufferState::None
                || tileUploadStagingBuffers[slot.buf].state == StagingBufferState::Completed)
               && "Upload slot data written while the buffer is in flight");

    if (tileUploadStagingBuffers[slot.buf].state == StagingBufferState::None
        || tileUploadStagingBuffers[slot.buf].state == StagingBufferState::Completed) {
        return tileUploadStagingBuffers[slot.buf].data + (slot.slot * TILE_SIZE);
    }

    return nullptr;
}

void Renderer::CommitTileUploadSlot(TileStageBufferSlot slot) {
    ZoneScoped;
    std::unique_lock lock(tileUploadStagingBuffers[slot.buf].mutex);
    if (tileUploadStagingBuffers[slot.buf].state == StagingBufferState::None
        || tileUploadStagingBuffers[slot.buf].state == StagingBufferState::Completed) {
        tileUploadStagingBuffers[slot.buf].commited[slot.slot] = true;
    }
}

void Renderer::FreeTileUploadSlot(TileStageBufferSlot slot) {
    ZoneScoped;
    SDL_assert(slot.buf < std::size(tileUploadStagingBuffers) && "Upload slot buffer index out of range");
    SDL_assert(slot.slot < TILE_UPLOAD_BUFFER_CAPACITY && "Upload slot index out of range");

    std::unique_lock lock(tileUploadStagingBuffers[slot.buf].mutex);
    SDL_assert(tileUploadStagingBuffers[slot.buf].used[slot.slot] && "Freeing an upload slot that was not reserved");
    SDL_assert(tileUploadStagingBuffers[slot.buf].state != StagingBufferState::Transfering
               && "Freeing an upload slot while the buffer is in flight");

    if (tileUploadStagingBuffers[slot.buf].state != StagingBufferState::Transfering) {
        tileUploadStagingBuffers[slot.buf].commited[slot.slot] = false;
        tileUploadStagingBuffers[slot.buf].used[slot.slot] = false;

        if (tileUploadStagingBuffers[slot.buf].state == StagingBufferState::Completed) {
            tileUploadStagingBuffers[slot.buf].state = StagingBufferState::None;
        } else if (tileUploadStagingBuffers[slot.buf].state == StagingBufferState::Done) {
            if (tileUploadStagingBuffers[slot.buf].used.none()) {
                SDL_assert(tileUploadStagingBuffers[slot.buf].commited.none());
                tileUploadStagingBuffers[slot.buf].state = StagingBufferState::None;
            }
        }
    }
}

std::expected<TileStageBufferSlot, Error> Renderer::QueueTileDownloadSlot(TileTransfer::ID id) {
    ZoneScoped;
    for (uint8_t buf = 0; buf < static_cast<uint8_t>(std::size(tileDownloadStagingBuffers)); buf++) {
        std::unique_lock lock(tileDownloadStagingBuffers[buf].mutex);
        if (tileDownloadStagingBuffers[buf].state == StagingBufferState::None) {
            if (!tileDownloadStagingBuffers[buf].used.all()) {
                for (uint8_t slot = 0; slot < static_cast<uint8_t>(tileDownloadStagingBuffers[buf].used.size());
                     slot++) {
                    if (!tileDownloadStagingBuffers[buf].used[slot]) {
                        SDL_assert(!tileDownloadStagingBuffers[buf].commited[slot]
                                   && "Queueing a download slot that is still marked committed");
                        tileDownloadStagingBuffers[buf].used[slot] = true;
                        tileDownloadStagingBuffers[buf].commited[slot] = true;
                        tileDownloadStagingBuffers[buf].ids[slot] = id;

                        if (tileDownloadStagingBuffers[buf].used.all()
                            && tileDownloadStagingBuffers[buf].commited.all()) {
                            tileDownloadStagingBuffers[buf].state = StagingBufferState::Completed;
                        }

                        SDL_assert(tileDownloadStagingBuffers[buf].used.all()
                                       == (tileDownloadStagingBuffers[buf].state == StagingBufferState::Completed)
                                   && "Download buffer state does not match its slot occupancy");

                        return TileStageBufferSlot{
                            .buf = buf,
                            .slot = slot,
                        };
                    }
                }
            }
        }
    }

    return std::unexpected(Error::OutOfCapacity);
}

std::expected<TileStageBufferSlot, Error> Renderer::GetTileDownloadSlot(TileTransfer::ID id) {
    ZoneScoped;
    for (uint8_t buf = 0; buf < static_cast<uint8_t>(std::size(tileDownloadStagingBuffers)); buf++) {
        std::unique_lock lock(tileDownloadStagingBuffers[buf].mutex);
        for (uint8_t slot = 0; slot < static_cast<uint8_t>(tileDownloadStagingBuffers[buf].used.size()); slot++) {
            if (tileDownloadStagingBuffers[buf].used[slot] && tileDownloadStagingBuffers[buf].ids[slot] == id) {
                return TileStageBufferSlot{
                    .buf = buf,
                    .slot = slot,
                };
            }
        }
    }
    return std::unexpected(Error::Unknown);
}

void* Renderer::GetTileDownloadSlotData(TileStageBufferSlot slot) {
    ZoneScoped;
    SDL_assert(slot.buf < std::size(tileDownloadStagingBuffers) && "Download slot buffer index out of range");
    SDL_assert(slot.slot < TILE_DOWNLOAD_BUFFER_CAPACITY && "Download slot index out of range");

    std::unique_lock lock(tileDownloadStagingBuffers[slot.buf].mutex);
    SDL_assert(tileDownloadStagingBuffers[slot.buf].used[slot.slot] && "Download slot was not queued");
    SDL_assert(tileDownloadStagingBuffers[slot.buf].state == StagingBufferState::Done
               && "Download slot read before the gpu fence signalled");

    if (tileDownloadStagingBuffers[slot.buf].state == StagingBufferState::Done) {
        return tileDownloadStagingBuffers[slot.buf].data + (slot.slot * TILE_SIZE);
    }

    return nullptr;
}

void Renderer::FreeTileDownloadSlot(TileStageBufferSlot slot) {
    ZoneScoped;
    SDL_assert(slot.buf < std::size(tileDownloadStagingBuffers) && "Download slot buffer index out of range");
    SDL_assert(slot.slot < TILE_DOWNLOAD_BUFFER_CAPACITY && "Download slot index out of range");

    std::unique_lock lock(tileDownloadStagingBuffers[slot.buf].mutex);
    SDL_assert(tileDownloadStagingBuffers[slot.buf].used[slot.slot] && "Freeing a download slot that was not queued");
    SDL_assert(tileDownloadStagingBuffers[slot.buf].state == StagingBufferState::Done
               && "Freeing a download slot before the gpu fence signalled");

    tileDownloadStagingBuffers[slot.buf].commited[slot.slot] = false;
    tileDownloadStagingBuffers[slot.buf].used[slot.slot] = false;

    if (tileDownloadStagingBuffers[slot.buf].used.none()) {
        SDL_assert(tileDownloadStagingBuffers[slot.buf].commited.none());
        tileDownloadStagingBuffers[slot.buf].state = StagingBufferState::None;
    }
}

Renderer::Renderer(App* app) : app(app) {}

bool Renderer::Init() {
    ZoneScoped;

#ifdef NDEBUG
    constexpr bool DEBUG_MODE = false;
#else
    constexpr bool DEBUG_MODE = true;
#endif // NDEBUG

#if defined(MIDORI_WINDOWS)
    constexpr SDL_GPUShaderFormat SUPPORTED_FORMATS = SDL_GPU_SHADERFORMAT_SPIRV;
#elif defined(MIDORI_LINUX)
    constexpr SDL_GPUShaderFormat supportedFormats = SDL_GPU_SHADERFORMAT_SPIRV;
#else
    static_assert(false, "Unsupported platform");
#endif

    device = SDL_CreateGPUDevice(SUPPORTED_FORMATS, DEBUG_MODE, nullptr);
    if (device == nullptr) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to create gpu device: %s", SDL_GetError());
        return false;
    }

    const char* deviceDriver = SDL_GetGPUDeviceDriver(device);
    if (strcmp(deviceDriver, "direct3d12") == 0) {
        SDL_Log("Backend: %s", deviceDriver);
        shaderFormat = SDL_GPU_SHADERFORMAT_DXIL;
    } else if (strcmp(deviceDriver, "vulkan") == 0) {
        SDL_Log("Backend: %s", deviceDriver);
        shaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;
    }

    if (!SDL_ClaimWindowForGPUDevice(device, app->window)) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to create gpu device: %s", SDL_GetError());
        return false;
    }
    SDL_SetGPUSwapchainParameters(device, app->window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
    SDL_SetGPUAllowedFramesInFlight(device, 1);

    swapchainFormat = SDL_GetGPUSwapchainTextureFormat(device, app->window);

    viewportRenderData.projection =
        glm::ortho(-std::floor((float)app->windowSize.x / 2.0f), std::ceil((float)app->windowSize.x / 2.0f),
                   -std::floor((float)app->windowSize.y / 2.0f), std::ceil((float)app->windowSize.y / 2.0f));

    if (!InitMerge()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize merge compute pipeline");
        return false;
    }

    if (!InitPaint()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize paint compute pipeline");
        return false;
    }

    if (!InitLayers()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize renderer layers");
        return false;
    }

    if (!InitTiles()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize tiles");
    }

    return true;
}

bool Renderer::InitLayers() {
    ZoneScoped;
    const SDL_GPUShaderCreateInfo vertexShaderCreateInfo = {
        .code_size = sizeof(shader_layer_vert),
        .code = reinterpret_cast<const Uint8*>(shader_layer_vert),
        .entrypoint = "main",
        .format = shaderFormat,
        .stage = SDL_GPU_SHADERSTAGE_VERTEX,
        .num_samplers = 0,
        .num_storage_textures = 0,
        .num_storage_buffers = 0,
        .num_uniform_buffers = 1,
    };
    auto* layerVertexShader = SDL_CreateGPUShader(device, &vertexShaderCreateInfo);
    if (layerVertexShader == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create layer vertex shader: %s", SDL_GetError());
    }

    const SDL_GPUShaderCreateInfo fragmentShaderCreateInfo = {
        .code_size = sizeof(shader_layer_frag),
        .code = reinterpret_cast<const Uint8*>(shader_layer_frag),
        .entrypoint = "main",
        .format = shaderFormat,
        .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
        .num_samplers = 1,
        .num_storage_textures = 0,
        .num_storage_buffers = 0,
        .num_uniform_buffers = 0,
    };
    auto* layerFragmentShader = SDL_CreateGPUShader(device, &fragmentShaderCreateInfo);
    if (layerFragmentShader == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create layer fragment shader: %s", SDL_GetError());
    }

    SDL_GPUColorTargetDescription colorTargetDescriptions[] = {{
        .format = swapchainFormat,
        .blend_state =
            {
                .src_color_blendfactor = SDL_GPU_BLENDFACTOR_INVALID,
                .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_INVALID,
                .color_blend_op = SDL_GPU_BLENDOP_INVALID,
                .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_INVALID,
                .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_INVALID,
                .alpha_blend_op = SDL_GPU_BLENDOP_INVALID,
                .color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B
                                  | SDL_GPU_COLORCOMPONENT_A,
                .enable_blend = false,
                .enable_color_write_mask = false,
            },
    }};

    const SDL_GPUGraphicsPipelineCreateInfo pipelineCreateInfo = {
        .vertex_shader = layerVertexShader,
        .fragment_shader = layerFragmentShader,
        .vertex_input_state =
            {
                .vertex_buffer_descriptions = nullptr,
                .num_vertex_buffers = 0,
                .vertex_attributes = nullptr,
                .num_vertex_attributes = 0,
            },
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state =
            {
                .fill_mode = SDL_GPU_FILLMODE_FILL,
                .cull_mode = SDL_GPU_CULLMODE_BACK,
                .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
                .depth_bias_constant_factor = 0.0f,
                .depth_bias_clamp = 0.0f,
                .depth_bias_slope_factor = 0.0f,
                .enable_depth_bias = false,
                .enable_depth_clip = false,
            },
        .multisample_state =
            {
                .sample_count = SDL_GPU_SAMPLECOUNT_1,
                .sample_mask = 0,
                .enable_mask = false,
            },
        .depth_stencil_state =
            {
                .compare_op = SDL_GPU_COMPAREOP_INVALID,
                .back_stencil_state = {.fail_op = SDL_GPU_STENCILOP_INVALID},
                .front_stencil_state = {.fail_op = SDL_GPU_STENCILOP_INVALID},
                .compare_mask = 0,
                .write_mask = 0,
                .enable_depth_test = false,
                .enable_depth_write = false,
                .enable_stencil_test = false,
            },
        .target_info =
            {
                .color_target_descriptions = colorTargetDescriptions,
                .num_color_targets = 1,
                .depth_stencil_format = SDL_GPU_TEXTUREFORMAT_INVALID,
                .has_depth_stencil_target = false,
            },

    };
    layerGraphicsPipeline = SDL_CreateGPUGraphicsPipeline(device, &pipelineCreateInfo);
    if (layerGraphicsPipeline == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to create layer graphics pipeline: %s", SDL_GetError());
        return false;
    }

    SDL_ReleaseGPUShader(device, layerFragmentShader);
    SDL_ReleaseGPUShader(device, layerVertexShader);

    const SDL_GPUSamplerCreateInfo samplerCreateInfo = {
        .min_filter = SDL_GPU_FILTER_NEAREST,
        .mag_filter = SDL_GPU_FILTER_NEAREST,
        .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
        .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .mip_lod_bias = 0.0f,
        .max_anisotropy = 0.0f,
        .compare_op = SDL_GPU_COMPAREOP_INVALID,
        .min_lod = 0.0f,
        .max_lod = 0.0f,
        .enable_anisotropy = false,
        .enable_compare = false,
    };
    layerSampler = SDL_CreateGPUSampler(device, &samplerCreateInfo);
    if (layerSampler == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to create layer sampler: %s", SDL_GetError());
        return false;
    }

    const SDL_GPUTextureCreateInfo canvasTextureCreateInfo = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = textureFormat,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
               | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE,
        .width = static_cast<Uint32>(app->windowSize.x),
        .height = static_cast<Uint32>(app->windowSize.y),
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    canvasViewTexture = SDL_CreateGPUTexture(device, &canvasTextureCreateInfo);
    if (canvasViewTexture == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to base canvas texture");
        return false;
    }

    return true;
}

bool Renderer::InitTiles() {
    ZoneScoped;
    const SDL_GPUShaderCreateInfo vertexShaderCreateInfo = {
        .code_size = sizeof(shader_tile_vert),
        .code = reinterpret_cast<const Uint8*>(shader_tile_vert),
        .entrypoint = "main",
        .format = shaderFormat,
        .stage = SDL_GPU_SHADERSTAGE_VERTEX,
        .num_samplers = 0,
        .num_storage_textures = 0,
        .num_storage_buffers = 0,
        .num_uniform_buffers = 2,
    };
    auto* tileVertexShader = SDL_CreateGPUShader(device, &vertexShaderCreateInfo);
    if (tileVertexShader == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create tile vertex shader: %s", SDL_GetError());
    }

    const SDL_GPUShaderCreateInfo fragmentShaderCreateInfo = {
        .code_size = sizeof(shader_tile_frag),
        .code = reinterpret_cast<const Uint8*>(shader_tile_frag),
        .entrypoint = "main",
        .format = shaderFormat,
        .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
        .num_samplers = 1,
        .num_storage_textures = 0,
        .num_storage_buffers = 0,
        .num_uniform_buffers = 0,
    };
    auto* tileFragmentShader = SDL_CreateGPUShader(device, &fragmentShaderCreateInfo);
    if (tileFragmentShader == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create tile fragment shader: %s", SDL_GetError());
    }

    SDL_GPUColorTargetDescription colorTargetDescriptions[] = {{
        .format = textureFormat,
        .blend_state =
            {
                .src_color_blendfactor = SDL_GPU_BLENDFACTOR_INVALID,
                .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_INVALID,
                .color_blend_op = SDL_GPU_BLENDOP_INVALID,
                .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_INVALID,
                .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_INVALID,
                .alpha_blend_op = SDL_GPU_BLENDOP_INVALID,
                .color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B
                                  | SDL_GPU_COLORCOMPONENT_A,
                .enable_blend = false,
                .enable_color_write_mask = false,
            },
    }};

    const SDL_GPUGraphicsPipelineCreateInfo pipelineCreateInfo = {
        .vertex_shader = tileVertexShader,
        .fragment_shader = tileFragmentShader,
        .vertex_input_state =
            {
                .vertex_buffer_descriptions = nullptr,
                .num_vertex_buffers = 0,
                .vertex_attributes = nullptr,
                .num_vertex_attributes = 0,
            },
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP,
        .rasterizer_state =
            {
                .fill_mode = SDL_GPU_FILLMODE_FILL,
                .cull_mode = SDL_GPU_CULLMODE_NONE,
                .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
                .depth_bias_constant_factor = 0.0f,
                .depth_bias_clamp = 0.0f,
                .depth_bias_slope_factor = 0.0f,
                .enable_depth_bias = false,
                .enable_depth_clip = false,

            },
        .multisample_state =
            {
                .sample_count = SDL_GPU_SAMPLECOUNT_1,
                .sample_mask = 0,
                .enable_mask = false,
            },
        .depth_stencil_state =
            {
                .compare_op = SDL_GPU_COMPAREOP_INVALID,
                .back_stencil_state = {.fail_op = SDL_GPU_STENCILOP_INVALID},
                .front_stencil_state = {.fail_op = SDL_GPU_STENCILOP_INVALID},
                .compare_mask = 0,
                .write_mask = 0,
                .enable_depth_test = false,
                .enable_depth_write = false,
                .enable_stencil_test = false,
            },
        .target_info =
            {
                .color_target_descriptions = colorTargetDescriptions,
                .num_color_targets = 1,
                .depth_stencil_format = SDL_GPU_TEXTUREFORMAT_INVALID,
                .has_depth_stencil_target = false,
            },

    };
    tileGraphicsPipeline = SDL_CreateGPUGraphicsPipeline(device, &pipelineCreateInfo);
    if (tileGraphicsPipeline == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to create tile graphics pipeline: %s", SDL_GetError());
        return false;
    }
    SDL_ReleaseGPUShader(device, tileFragmentShader);
    SDL_ReleaseGPUShader(device, tileVertexShader);

    const SDL_GPUSamplerCreateInfo samplerCreateInfo = {
        .min_filter = SDL_GPU_FILTER_LINEAR,
        .mag_filter = SDL_GPU_FILTER_LINEAR,
        .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR,
        .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .mip_lod_bias = 1.0f,
        .max_anisotropy = 8.0f,
        .compare_op = SDL_GPU_COMPAREOP_INVALID,
        .min_lod = 0.0f,
        .max_lod = 4.0f,
        .enable_anisotropy = true,
        .enable_compare = false,
    };
    tileSampler = SDL_CreateGPUSampler(device, &samplerCreateInfo);
    if (tileSampler == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to create tile sampler: %s", SDL_GetError());
        return false;
    }

    const SDL_GPUTransferBufferCreateInfo uploadBufferCreateInfo = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = TILE_UPLOAD_BUFFER_CAPACITY * TILE_SIZE,
    };

    for (auto& buf : tileUploadStagingBuffers) {
        buf.buffer = SDL_CreateGPUTransferBuffer(device, &uploadBufferCreateInfo);
        if (buf.buffer == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create tile upload buffer: %s", SDL_GetError());
            return false;
        }
        buf.data = static_cast<std::byte*>(SDL_MapGPUTransferBuffer(device, buf.buffer, false));
        if (buf.data == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to map tile upload transfer buffer: %s", SDL_GetError());
            return false;
        }
    }

    const SDL_GPUTransferBufferCreateInfo downloadBufferCreateInfo = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        .size = TILE_DOWNLOAD_BUFFER_CAPACITY * TILE_SIZE,
    };
    for (auto& buf : tileDownloadStagingBuffers) {
        buf.buffer = SDL_CreateGPUTransferBuffer(device, &uploadBufferCreateInfo);
        if (buf.buffer == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create tile upload buffer: %s", SDL_GetError());
            return false;
        }
        buf.data = static_cast<std::byte*>(SDL_MapGPUTransferBuffer(device, buf.buffer, false));
        if (buf.data == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to map tile upload transfer buffer: %s", SDL_GetError());
            return false;
        }
    }

    const SDL_GPUTransferBufferCreateInfo tileBlankTextureBufferCreateInfo = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = TILE_SIZE,
    };
    tileBlankTextureBuffer = SDL_CreateGPUTransferBuffer(device, &tileBlankTextureBufferCreateInfo);
    if (tileBlankTextureBuffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create tile upload buffer: %s", SDL_GetError());
        return false;
    }

    auto* buf = static_cast<std::byte*>(SDL_MapGPUTransferBuffer(device, tileBlankTextureBuffer, false));
    std::memset(buf, 0, TILE_SIZE);
    SDL_UnmapGPUTransferBuffer(device, tileBlankTextureBuffer);

    return true;
}

bool Renderer::InitMerge() {
    const SDL_GPUComputePipelineCreateInfo createInfo = {
        .code_size = sizeof(shader_merge_comp),
        .code = reinterpret_cast<const Uint8*>(shader_merge_comp),
        .entrypoint = "main",
        .format = shaderFormat,
        .num_samplers = 1,
        .num_readonly_storage_textures = 0,  // src texture
        .num_readonly_storage_buffers = 0,
        .num_readwrite_storage_textures = 1, // dst texture
        .num_readwrite_storage_buffers = 0,
        .num_uniform_buffers = 1,
        .threadcount_x = 32,
        .threadcount_y = 32,
        .threadcount_z = 1,
    };
    mergeComputePipeline = SDL_CreateGPUComputePipeline(device, &createInfo);
    if (mergeComputePipeline == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize merge compute pipeline: %s", SDL_GetError());
        return false;
    }

    return true;
}

bool Renderer::InitPaint() {
    const SDL_GPUComputePipelineCreateInfo paintCreateInfo = {
        .code_size = sizeof(shader_paint_comp),
        .code = reinterpret_cast<const Uint8*>(shader_paint_comp),
        .entrypoint = "main",
        .format = shaderFormat,
        .num_samplers = 1,                   // alpha brush
        .num_readonly_storage_textures = 0,
        .num_readonly_storage_buffers = 1,   // stroke buffer
        .num_readwrite_storage_textures = 1, // dst texture
        .num_readwrite_storage_buffers = 0,
        .num_uniform_buffers = 2,            // tile + stroke
        .threadcount_x = 32,
        .threadcount_y = 32,
        .threadcount_z = 1,
    };
    paintComputePipeline = SDL_CreateGPUComputePipeline(device, &paintCreateInfo);
    if (paintComputePipeline == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize paint compute pipeline: %s", SDL_GetError());
        return false;
    }

    const SDL_GPUComputePipelineCreateInfo eraseCreateInfo = {
        .code_size = sizeof(shader_erase_comp),
        .code = reinterpret_cast<const Uint8*>(shader_erase_comp),
        .entrypoint = "main",
        .format = shaderFormat,
        .num_samplers = 1,                   // alpha brush
        .num_readonly_storage_textures = 0,
        .num_readonly_storage_buffers = 1,   // stroke buffer
        .num_readwrite_storage_textures = 1, // dst texture
        .num_readwrite_storage_buffers = 0,
        .num_uniform_buffers = 2,            // tile + stroke
        .threadcount_x = 32,
        .threadcount_y = 32,
        .threadcount_z = 1,
    };
    eraseComputePipeline = SDL_CreateGPUComputePipeline(device, &eraseCreateInfo);
    if (eraseComputePipeline == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize erase compute pipeline: %s", SDL_GetError());
        return false;
    }

    const SDL_GPUBufferCreateInfo bufferCreateInfo = {
        .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
        .size = STROKE_MAX_POINTS * sizeof(StrokePoint),
    };
    paintStrokePointBuffer = SDL_CreateGPUBuffer(device, &bufferCreateInfo);
    if (paintStrokePointBuffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create paint stroke buffer: %s", SDL_GetError());
        return false;
    }

    const SDL_GPUTransferBufferCreateInfo uploadBufferCreateInfo = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = STROKE_MAX_POINTS * sizeof(StrokePoint),
    };
    paintStrokePointTransferBuffer = SDL_CreateGPUTransferBuffer(device, &uploadBufferCreateInfo);
    if (paintStrokePointTransferBuffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create paint stroke upload buffer: %s", SDL_GetError());
        return false;
    }
    paintStrokePointTransferBufferPtr =
        static_cast<std::byte*>(SDL_MapGPUTransferBuffer(device, paintStrokePointTransferBuffer, false));

#ifdef NDEBUG
    const std::string path = SDL_GetPrefPath(nullptr, "midori");
#else
    const std::string path = SDL_GetPrefPath(nullptr, "midori-dev");
#endif
    const std::string brushPath = std::format("{}brushes/sphere.qoi", path);

    size_t dataSize;
    auto* data = (uint8_t*)SDL_LoadFile(brushPath.c_str(), &dataSize);
    if (data == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read sphere brush texture: %s", SDL_GetError());
        return false;
    }

    qoi_desc desc;
    auto* pixels = (uint8_t*)qoi_decode(data, static_cast<int>(dataSize), &desc, 4);
    SDL_free(data);
    if (pixels == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to decode brush texture");
        return false;
    }

    // for (size_t i = 0; i < (size_t)desc.width * desc.height; i++) {
    //     pixels[i] = pixels[i * 3];
    // }

    SDL_GPUTextureCreateInfo brushTextureCreateInfo = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = textureFormat,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
        .width = static_cast<Uint32>(desc.width),
        .height = static_cast<Uint32>(desc.height),
        .layer_count_or_depth = 1,
        .num_levels = 10,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    brushTexture = SDL_CreateGPUTexture(device, &brushTextureCreateInfo);
    if (brushTexture == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create brush texture: %s", SDL_GetError());
        return false;
    }

    SDL_GPUTransferBufferCreateInfo transferBufferCreateInfo = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = desc.width * desc.height * 4,
    };
    SDL_GPUTransferBuffer* brushTransferBuffer = SDL_CreateGPUTransferBuffer(device, &transferBufferCreateInfo);
    if (brushTransferBuffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create temp brush transfer buffer: %s", SDL_GetError());
        return false;
    }

    auto* buf = static_cast<std::byte*>(SDL_MapGPUTransferBuffer(device, brushTransferBuffer, false));
    memcpy(buf, pixels, (size_t)desc.width * desc.height * 4);
    SDL_UnmapGPUTransferBuffer(device, brushTransferBuffer);
    free(pixels);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    if (commandBuffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
        return false;
    }
    SDL_GPUCopyPass* uploadPass = SDL_BeginGPUCopyPass(commandBuffer);
    const SDL_GPUTextureTransferInfo transferInfo = {
        .transfer_buffer = brushTransferBuffer,
        .pixels_per_row = desc.width,
        .rows_per_layer = desc.height,
    };
    const SDL_GPUTextureRegion textureRegion = {
        .texture = brushTexture,
        .w = desc.width,
        .h = desc.height,
        .d = 1,
    };
    SDL_UploadToGPUTexture(uploadPass, &transferInfo, &textureRegion, false);
    SDL_EndGPUCopyPass(uploadPass);
    SDL_GenerateMipmapsForGPUTexture(commandBuffer, brushTexture);
    if (!SDL_SubmitGPUCommandBuffer(commandBuffer)) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
        return false;
    }

    SDL_ReleaseGPUTransferBuffer(device, brushTransferBuffer);

    const SDL_GPUSamplerCreateInfo brushSamplerCreateInfo = {
        .min_filter = SDL_GPU_FILTER_LINEAR,
        .mag_filter = SDL_GPU_FILTER_LINEAR,
        .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR,
        .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
        .mip_lod_bias = 0.0f,
        .max_anisotropy = 0.0f,
        .compare_op = SDL_GPU_COMPAREOP_INVALID,
        .min_lod = 0.0f,
        .max_lod = 0.0f,
        .enable_anisotropy = false,
        .enable_compare = false,
    };
    brushSampler = SDL_CreateGPUSampler(device, &brushSamplerCreateInfo);
    if (brushSampler == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create brush sampler: %s", SDL_GetError());
        return false;
    }

    return true;
}

bool Renderer::Update() {
    ZoneScoped;

    UpdateTileTextureUpload();
    UpdateTileTextureDownload();

    if (!UpdateUninitializedTextures()) {
        return false;
    }

    return true;
}

bool Renderer::UpdateUninitializedTextures() {
    ZoneScoped;

    // All of those textures must be initialized
    if (!tileTexturesToInitialize.empty()) {
        SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(device);
        if (commandBuffer == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
            return false;
        }
        SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);

        for (const auto& tile : tileTexturesToInitialize) {
            if (!tileTextures.contains(tile)) {
                continue;
            }
            const SDL_GPUTextureTransferInfo transferInfo = {
                .transfer_buffer = tileBlankTextureBuffer,
                .pixels_per_row = TILE_WIDTH,
                .rows_per_layer = TILE_HEIGHT,
            };
            SDL_assert(tileTextures.at(tile) && "Tile texture is null");
            const SDL_GPUTextureRegion textureRegion = {
                .texture = tileTextures.at(tile),
                .mip_level = 0,
                .layer = 0,
                .x = 0,
                .y = 0,
                .z = 0,
                .w = TILE_WIDTH,
                .h = TILE_HEIGHT,
                .d = 1,
            };
            SDL_UploadToGPUTexture(copyPass, &transferInfo, &textureRegion, false);
        }

        SDL_EndGPUCopyPass(copyPass);

        auto* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
        if (!fence) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit command buffer: %s", SDL_GetError());
            return false;
        }

        // This is expected to be done before any rendering/painting so we wait for all uploads to be done
        // This could be done at a better place in the future like before actually needing to do something with the
        // tileTexture i.e: Merge, Paint. Render is not concerned since this is blank so we don't care we just don't
        // show the tile in this case. In SDL3GPU the UploadToGpuTexures can be expected to be done before an other call
        // so technically we don't care of the timeline but for completeness I still do it
        SDL_WaitForGPUFences(device, true, &fence, 1);
        SDL_ReleaseGPUFence(device, fence);

        for (const auto& tile : tileTexturesToInitialize) {
            SDL_assert(app->canvasFiles.at(tile.canvas).tileStates.contains(tile));
            app->canvasFiles.at(tile.canvas).tileStates.at(tile) |= TileState::Loaded;
        }

        tileTexturesToInitialize.clear();
    }

    return true;
}

Error Renderer::UpdateTileTextureUpload() {
    ZoneScoped;

    auto* commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    if (commandBuffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
        return Error::Unknown;
    }
    auto* copyPass = SDL_BeginGPUCopyPass(commandBuffer);

    bool recorded{};
    for (auto& buf : tileUploadStagingBuffers) {
        std::unique_lock lock(buf.mutex);
        if (buf.state == StagingBufferState::Completed) {
            SDL_assert(!buf.fence);
            if (buf.commited == buf.used) {
                for (size_t slot = 0; slot < buf.used.size(); slot++) {
                    if (buf.used[slot]) {
                        SDL_assert(buf.commited[slot] && "Uploading an upload staging slot that was never reserved");
                        SDL_assert(app->tileTransfers.contains(buf.ids[slot])
                                   && "Upload staging slot references a retired transfer");
                        auto& transfer = app->tileTransfers.at(buf.ids[slot]);
                        SDL_assert(transfer.read == TileReadState::Decoded
                                   && "Uploading a tile that was not decoded by a worker");
                        buf.state = StagingBufferState::Transfering;
                        const SDL_GPUTextureTransferInfo transferInfo = {
                            .transfer_buffer = buf.buffer,
                            .offset = static_cast<Uint32>(slot * TILE_SIZE),
                            .pixels_per_row = TILE_WIDTH,
                            .rows_per_layer = TILE_HEIGHT,
                        };
                        SDL_assert(!tileTextures.contains(transfer.tile) && "Old tile texture found");
                        CreateUninitializedTileTexture(transfer.tile);
                        SDL_assert(tileTextures.contains(transfer.tile) && "Tile texture not found");
                        const SDL_GPUTextureRegion textureRegion = {
                            .texture = tileTextures.at(transfer.tile),
                            .mip_level = 0,
                            .layer = 0,
                            .x = 0,
                            .y = 0,
                            .z = 0,
                            .w = TILE_WIDTH,
                            .h = TILE_HEIGHT,
                            .d = 1,
                        };
                        SDL_UploadToGPUTexture(copyPass, &transferInfo, &textureRegion, false);
                        // need to be done with atomics
                        transfer.read = TileReadState::Uploaded;
                        recorded = true;
                    }
                }
                buf.state = StagingBufferState::Transfering;
            }
        }
    }

    if (recorded) {
        SDL_EndGPUCopyPass(copyPass);
        const bool res = SDL_SubmitGPUCommandBuffer(commandBuffer);
        if (!res) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
            return Error::Unknown;
        }
    } else {
        SDL_CancelGPUCommandBuffer(commandBuffer);
    }

    for (auto& buf : tileUploadStagingBuffers) {
        if (buf.state == StagingBufferState::Transfering) {
            // Checking the fence is useless since per SDL doc the upload are done by the time the next command buffer
            // is issued

            for (size_t slot = 0; slot < buf.used.size(); slot++) {
                SDL_assert(buf.used[slot] == buf.commited[slot]);
                if (buf.used[slot]) {
                    SDL_assert(app->tileTransfers.contains(buf.ids[slot]));
                    auto& transfer = app->tileTransfers.at(buf.ids[slot]);
                    SDL_assert(transfer.write == TileWriteState::None);
                    SDL_assert(transfer.read == TileReadState::Uploaded);

                    SDL_assert(app->canvasFiles.contains(transfer.tile.canvas));
                    auto& canvasFile = app->canvasFiles.at(transfer.tile.canvas);

                    SDL_assert(canvasFile.layers.contains(transfer.tile.layer));
                    auto& tileState = canvasFile.tileStates.at(transfer.tile);
                    SDL_assert(!IsTileLoaded(tileState));
                    SDL_assert(IsTileTransfering(tileState));

                    transfer.read = TileReadState::Done;
                    tileState |= TileState::Loaded;
                    tileState &= ~TileState::Transfering;
                }
            }

            std::unique_lock lock(buf.mutex);
            // Release all the previous slots
            buf.state = StagingBufferState::None;
            buf.used.reset();
            buf.commited.reset();
        }
    }

    return Error::None;
}

Error Renderer::UpdateTileTextureDownload() {
    ZoneScoped;

    for (auto& buf : tileDownloadStagingBuffers) {
        std::unique_lock lock(buf.mutex);
        if (buf.state == StagingBufferState::Completed) {
            SDL_assert(!buf.fence);
            if (buf.commited == buf.used) {
                auto* commandBuffer = SDL_AcquireGPUCommandBuffer(device);
                if (commandBuffer == nullptr) {
                    SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
                    return Error::Unknown;
                }
                auto* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
                for (size_t slot = 0; slot < buf.used.size(); slot++) {
                    if (buf.used[slot]) {
                        SDL_assert(buf.commited[slot] && "Downloading a download staging slot that was never queued");
                        SDL_assert(app->tileTransfers.contains(buf.ids[slot])
                                   && "Download staging slot references a retired transfer");
                        auto& transfer = app->tileTransfers.at(buf.ids[slot]);
                        SDL_assert(transfer.write == TileWriteState::Pending);

                        buf.state = StagingBufferState::Transfering;
                        const SDL_GPUTextureTransferInfo transferInfo = {
                            .transfer_buffer = buf.buffer,
                            .offset = static_cast<Uint32>(slot * TILE_SIZE),
                            .pixels_per_row = TILE_WIDTH,
                            .rows_per_layer = TILE_HEIGHT,
                        };
                        SDL_assert(tileTextures.contains(transfer.tile) && "Tile texture not found");
                        const SDL_GPUTextureRegion textureRegion = {
                            .texture = tileTextures.at(transfer.tile),
                            .mip_level = 0,
                            .layer = 0,
                            .x = 0,
                            .y = 0,
                            .z = 0,
                            .w = TILE_WIDTH,
                            .h = TILE_HEIGHT,
                            .d = 1,
                        };

                        SDL_DownloadFromGPUTexture(copyPass, &textureRegion, &transferInfo);
                    }
                }

                SDL_EndGPUCopyPass(copyPass);
                buf.fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
                if (!buf.fence) {
                    SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
                    return Error::Unknown;
                }
                buf.state = StagingBufferState::Transfering;
            }
        } else if (buf.state == StagingBufferState::Transfering) {
            SDL_assert(buf.fence);
            if (SDL_QueryGPUFence(device, buf.fence)) {
                SDL_ReleaseGPUFence(device, buf.fence);
                buf.fence = nullptr;
                buf.state = StagingBufferState::Done;
                for (size_t slot = 0; slot < buf.used.size(); slot++) {
                    if (buf.used[slot]) {
                        SDL_assert(buf.commited[slot] && "Retiring a download staging slot that was never queued");
                        SDL_assert(app->tileTransfers.contains(buf.ids[slot])
                                   && "Download staging slot references a retired transfer");
                        auto& transfer = app->tileTransfers.at(buf.ids[slot]);
                        SDL_assert(transfer.write == TileWriteState::Pending);
                        // need to be done with atomics
                        transfer.write = TileWriteState::Downloaded;
                    }
                }
            }
        }
    }

    return Error::None;
}

Error Renderer::PaintTiles(const CanvasFile& canvasFile, std::span<Tile> tiles, std::span<StrokePoint> points) {
    ZoneScoped;
    SDL_assert(points.size() < STROKE_MAX_POINTS);
    SDL_assert(app->pendingTileEdits >= tiles.size() && "Painting more tiles than were recorded as pending");

    if (tiles.empty() || points.empty()) {
        return Error::None;
    }

    if (!UpdateUninitializedTextures()) {
        return Error::Unknown;
    }

    if (tileEditFence) {
        SDL_WaitForGPUFences(device, true, &tileEditFence, 1);
        SDL_ReleaseGPUFence(device, tileEditFence);
        tileEditFence = nullptr;
    }

    std::memset(paintStrokePointTransferBufferPtr, 0, STROKE_MAX_POINTS * sizeof(StrokePoint));
    std::memcpy(paintStrokePointTransferBufferPtr, points.data(), points.size_bytes());

    SDL_GPUCommandBuffer* cmdBuf = SDL_AcquireGPUCommandBuffer(device);
    if (!cmdBuf) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
        return Error::Unknown;
    }

    SDL_GPUCopyPass* strokeCopyPass = SDL_BeginGPUCopyPass(cmdBuf);

    const SDL_GPUTransferBufferLocation source = {
        .transfer_buffer = paintStrokePointTransferBuffer,
        .offset = 0,
    };
    const SDL_GPUBufferRegion destination = {
        .buffer = paintStrokePointBuffer,
        .offset = 0,
        .size = static_cast<Uint32>(points.size_bytes()),
    };

    SDL_UploadToGPUBuffer(strokeCopyPass, &source, &destination, true);
    SDL_EndGPUCopyPass(strokeCopyPass);

    const StrokeRenderData strokeRenderData = {
        .pointsNum = static_cast<std::uint32_t>(points.size()),
    };

    SDL_PushGPUComputeUniformData(cmdBuf, 0, &strokeRenderData, sizeof(StrokeRenderData));
    const glm::ivec2 paintComputeInvocations = glm::ceil(glm::vec2(TILE_WIDTH / 32.0f, TILE_HEIGHT / 32.0f));
    for (const auto& tile : tiles) {
        SDL_assert(tileTextures.contains(tile) && "Editing a tile that has no gpu texture");
        if (!tileTextures.contains(tile)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to get tile texture to paint");
            continue;
        }

        SDL_assert(tileTextures.at(tile) && "Tile texture is null");
        const SDL_GPUStorageTextureReadWriteBinding paintTileBinding[1] = {{
            .texture = tileTextures.at(tile),
            .mip_level = 0,
            .layer = 0,
        }};

        SDL_GPUComputePass* paintComputePass = SDL_BeginGPUComputePass(cmdBuf, paintTileBinding, 1, nullptr, 0);
        SDL_BindGPUComputePipeline(paintComputePass, paintComputePipeline);

        tileRenderData.position.x = tile.x;
        tileRenderData.position.y = tile.y;
        tileRenderData.size = glm::vec2(TILE_WIDTH, TILE_WIDTH);
        SDL_PushGPUComputeUniformData(cmdBuf, 1, &tileRenderData, sizeof(TileRenderData));

        SDL_GPUTextureSamplerBinding samplerBinding = {
            .texture = brushTexture,
            .sampler = brushSampler,
        };
        SDL_BindGPUComputeSamplers(paintComputePass, 0, &samplerBinding, 1);
        SDL_BindGPUComputeStorageBuffers(paintComputePass, 0, &paintStrokePointBuffer, 1);
        SDL_DispatchGPUCompute(paintComputePass, paintComputeInvocations.x, paintComputeInvocations.y, 1);
        SDL_EndGPUComputePass(paintComputePass);
    }

    tileEditFence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmdBuf);
    if (tileEditFence == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
        return Error::Unknown;
    }

    return Error::None;
}

Error Renderer::EraseTiles(const CanvasFile& canvasFile, std::span<Tile> tiles, std::span<StrokePoint> points) {
    ZoneScoped;
    SDL_assert(points.size() < STROKE_MAX_POINTS);
    SDL_assert(app->pendingTileEdits >= tiles.size() && "Erasing more tiles than were recorded as pending");

    if (tiles.empty() || points.empty()) {
        return Error::None;
    }

    if (!UpdateUninitializedTextures()) {
        return Error::None;
    }

    if (tileEditFence) {
        SDL_WaitForGPUFences(device, true, &tileEditFence, 1);
        SDL_ReleaseGPUFence(device, tileEditFence);
        tileEditFence = nullptr;
    }

    std::memset(paintStrokePointTransferBufferPtr, 0, STROKE_MAX_POINTS * sizeof(StrokePoint));
    std::memcpy(paintStrokePointTransferBufferPtr, points.data(), points.size_bytes());

    SDL_GPUCommandBuffer* cmdBuf = SDL_AcquireGPUCommandBuffer(device);
    if (!cmdBuf) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
        return Error::Unknown;
    }

    SDL_GPUCopyPass* strokeCopyPass = SDL_BeginGPUCopyPass(cmdBuf);

    const SDL_GPUTransferBufferLocation source = {
        .transfer_buffer = paintStrokePointTransferBuffer,
        .offset = 0,
    };
    const SDL_GPUBufferRegion destination = {
        .buffer = paintStrokePointBuffer,
        .offset = 0,
        .size = static_cast<Uint32>(points.size_bytes()),
    };

    SDL_UploadToGPUBuffer(strokeCopyPass, &source, &destination, true);
    SDL_EndGPUCopyPass(strokeCopyPass);

    const StrokeRenderData strokeRenderData = {
        .pointsNum = static_cast<std::uint32_t>(points.size()),
    };

    SDL_PushGPUComputeUniformData(cmdBuf, 0, &strokeRenderData, sizeof(StrokeRenderData));
    const glm::ivec2 paintComputeInvocations = glm::ceil(glm::vec2(TILE_WIDTH / 32.0f, TILE_HEIGHT / 32.0f));
    for (const auto& tile : tiles) {
        SDL_assert(tileTextures.contains(tile) && "Editing a tile that has no gpu texture");
        if (!tileTextures.contains(tile)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to get tile texture to paint");
            continue;
        }

        SDL_assert(tileTextures.at(tile) && "Tile texture is null");
        const SDL_GPUStorageTextureReadWriteBinding paintTileBinding[1] = {{
            .texture = tileTextures.at(tile),
            .mip_level = 0,
            .layer = 0,
        }};

        SDL_GPUComputePass* eraseComputePass = SDL_BeginGPUComputePass(cmdBuf, paintTileBinding, 1, nullptr, 0);
        SDL_BindGPUComputePipeline(eraseComputePass, eraseComputePipeline);

        tileRenderData.position.x = tile.x;
        tileRenderData.position.y = tile.y;
        tileRenderData.size = glm::vec2(TILE_WIDTH, TILE_HEIGHT);
        SDL_PushGPUComputeUniformData(cmdBuf, 1, &tileRenderData, sizeof(TileRenderData));

        SDL_GPUTextureSamplerBinding samplerBinding = {
            .texture = brushTexture,
            .sampler = brushSampler,
        };
        SDL_BindGPUComputeSamplers(eraseComputePass, 0, &samplerBinding, 1);
        SDL_BindGPUComputeStorageBuffers(eraseComputePass, 0, &paintStrokePointBuffer, 1);
        SDL_DispatchGPUCompute(eraseComputePass, paintComputeInvocations.x, paintComputeInvocations.y, 1);
        SDL_EndGPUComputePass(eraseComputePass);
    }

    if (tileEditFence) {
        SDL_WaitForGPUFences(device, true, &tileEditFence, 1);
        SDL_ReleaseGPUFence(device, tileEditFence);
        tileEditFence = nullptr;
    }
    tileEditFence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmdBuf);
    if (tileEditFence == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
        return Error::Unknown;
    }

    return Error::None;
}

Error Renderer::RenderCanvasView(CanvasView& canvasView) {
    ZoneScoped;

    SDL_GPUCommandBuffer* commandBuffer = nullptr;

    auto& canvasFile = app->canvasFiles.at(canvasView.canvasFileId);
    viewportRenderData.view = canvasView.viewport.ViewMatrix();

    commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    if (commandBuffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
        return Error::Unknown;
    }

    if (!tileToDraw.empty()) {
        ZoneScopedN("Rendering Tile");
        for (const auto& layer : canvasFile.layersTree) {
            const auto& layerInfo = canvasFile.layers.at(layer);
            if (layerInfo.hidden || layerInfo.opacity <= 0.0f) {
                continue;
            }

            ZoneScopedN("Render Layer Tiles");

            const SDL_GPUColorTargetInfo targetInfo = {
                .texture = layerTextures.at(layer),
                .clear_color = SDL_FColor{.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f},
                .load_op = SDL_GPU_LOADOP_CLEAR,
                .store_op = SDL_GPU_STOREOP_STORE,
            };
            SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(commandBuffer, &targetInfo, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(renderPass, tileGraphicsPipeline);

            SDL_PushGPUVertexUniformData(commandBuffer, 0, &viewportRenderData, sizeof(ViewportRenderData));

            for (auto [tile, state] : canvasFile.tileStates) {
                if (tileToDraw.contains(tile)) {
                    if (tileTextures.at(tile)) {
                        tileRenderData.position.x = tile.x;
                        tileRenderData.position.y = tile.y;
                        tileRenderData.size = glm::vec2(TILE_WIDTH, TILE_HEIGHT);
                        SDL_PushGPUVertexUniformData(commandBuffer, 1, &tileRenderData, sizeof(TileRenderData));

                        const SDL_GPUTextureSamplerBinding samplers[] = {{
                            .texture = tileTextures.at(tile),
                            .sampler = tileSampler,
                        }};
                        SDL_BindGPUFragmentSamplers(renderPass, 0, samplers, 1);
                        SDL_DrawGPUPrimitives(renderPass, 4, 1, 0, 0);
                    }
                }
            }
            SDL_EndGPURenderPass(renderPass);
        }
        tileToDraw.clear();
    }

    { // Layer rendering
        ZoneScopedN("Layer blending and rendering");

        auto rgb = glm::vec3(app->bgColor);
        rgb = glm::mix(glm::pow((rgb + glm::vec3(0.055f)) * glm::vec3(1.0f / 1.055f), glm::vec3(2.4f)),
                       rgb * glm::vec3(1.0f / 12.92f), glm::lessThanEqual(rgb, glm::vec3(0.04045f)));
        rgb *= app->bgColor.a;

        const SDL_GPUColorTargetInfo targetInfo = {
            .texture = canvasViewTexture,
            .clear_color = SDL_FColor{.r = rgb.r, .g = rgb.g, .b = rgb.b, .a = app->bgColor.a},
            .load_op = SDL_GPU_LOADOP_CLEAR,
            .store_op = SDL_GPU_STOREOP_STORE,
        };
        SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(commandBuffer, &targetInfo, 1, nullptr);
        SDL_EndGPURenderPass(renderPass);

        const glm::ivec2 mergeComputeInvocations = glm::ceil(glm::vec2(app->windowSize) / 32.0f);
        MergeRenderData mergeRenderData = {
            .srcBlendMode = static_cast<std::uint32_t>(BlendMode::Alpha),
            .srcOpacity = 1.0f,
        };
        const SDL_GPUStorageTextureReadWriteBinding mergeLayerBinding[1] = {{
            .texture = canvasViewTexture,
            .mip_level = 0,
            .layer = 0,
        }};
        SDL_GPUComputePass* mergeComputePass = SDL_BeginGPUComputePass(commandBuffer, mergeLayerBinding, 1, nullptr, 0);
        SDL_BindGPUComputePipeline(mergeComputePass, mergeComputePipeline);

        for (const auto& layer : canvasFile.layersTree) {
            const auto& layerInfo = canvasFile.layers.at(layer);
            if (layerInfo.hidden || layerInfo.opacity <= 0.0f) {
                continue;
            }

            ZoneScopedN("Blend layer to canvas");
            mergeRenderData.srcBlendMode = static_cast<std::uint32_t>(layerInfo.blendMode);
            mergeRenderData.srcOpacity = layerInfo.opacity;
            SDL_PushGPUComputeUniformData(commandBuffer, 0, &mergeRenderData, sizeof(MergeRenderData));

            const SDL_GPUTextureSamplerBinding samplers[] = {{
                .texture = layerTextures.at(layer),
                .sampler = layerSampler,
            }};
            SDL_BindGPUComputeSamplers(mergeComputePass, 0, samplers, 1);
            SDL_DispatchGPUCompute(mergeComputePass, mergeComputeInvocations.x, mergeComputeInvocations.y, 1);
        }

        SDL_EndGPUComputePass(mergeComputePass);
    }

    if (!SDL_SubmitGPUCommandBuffer(commandBuffer)) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
        return Error::Unknown;
    }

    return Error::Unknown;
}

Error Renderer::Render() {
    ZoneScoped;

    if (app->windowSize.x > 0 && app->windowSize.y > 0 && !app->hidden) {
        SDL_GPUCommandBuffer* commandBuffer = nullptr;
        SDL_GPUTexture* swapchainTexture = nullptr;

        commandBuffer = SDL_AcquireGPUCommandBuffer(device);
        if (commandBuffer == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
            return Error::Unknown;
        }

        { // Wait and acquire GPU swapchain texture
            ZoneScopedN("Wait and acquire GPU swapchain texture");
            SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, app->window, &swapchainTexture, nullptr, nullptr);
            if (swapchainTexture == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire swapchain texture: %s", SDL_GetError());
                return Error::Unknown;
            }
        }
        {
            ZoneScopedN("Render canvas texture");
            auto rgb = glm::vec3(app->bgColor);
            rgb = glm::mix(glm::pow((rgb + glm::vec3(0.055f)) * glm::vec3(1.0f / 1.055f), glm::vec3(2.4f)),
                           rgb * glm::vec3(1.0f / 12.92f), glm::lessThanEqual(rgb, glm::vec3(0.04045f)));
            rgb *= app->bgColor.a;

            const SDL_GPUColorTargetInfo targetInfo = {
                .texture = swapchainTexture,
                .clear_color = SDL_FColor{.r = rgb.r, .g = rgb.g, .b = rgb.b, .a = app->bgColor.a},
                .load_op = SDL_GPU_LOADOP_CLEAR,
                .store_op = SDL_GPU_STOREOP_STORE,
            };
            SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(commandBuffer, &targetInfo, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(renderPass, layerGraphicsPipeline);

            const SDL_GPUTextureSamplerBinding samplers[] = {{
                .texture = canvasViewTexture,
                .sampler = layerSampler,
            }};
            SDL_BindGPUFragmentSamplers(renderPass, 0, samplers, 1);
            SDL_DrawGPUPrimitives(renderPass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(renderPass);
        }

        { // UI Rendering
            ZoneScopedN("UI rendering");
            ImDrawData* drawData = ImGui::GetDrawData();

            ImGui_ImplSDLGPU3_PrepareDrawData(drawData, commandBuffer);
            const SDL_GPUColorTargetInfo targetInfo = {
                .texture = swapchainTexture,
                .load_op = SDL_GPU_LOADOP_LOAD,
                .store_op = SDL_GPU_STOREOP_STORE,
            };
            SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(commandBuffer, &targetInfo, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(drawData, commandBuffer, renderPass);
            SDL_EndGPURenderPass(renderPass);
        }

        {
            ZoneScopedN("Submiting GPU command buffer");
            auto* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
            if (fence == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
                return Error::Unknown;
            }
            SDL_WaitForGPUFences(device, true, &fence, 1);
            SDL_ReleaseGPUFence(device, fence);
        }
    }

    return Error::None;
}

Error Renderer::ResizeCanvasView(const CanvasView& canvasView) {
    ZoneScoped;

    viewportRenderData.projection = glm::ortho(-std::floor(static_cast<float>(canvasView.width) / 2.0f),
                                               std::ceil(static_cast<float>(canvasView.width) / 2.0f),
                                               -std::floor(static_cast<float>(canvasView.height) / 2.0f),
                                               std::ceil(static_cast<float>(canvasView.height) / 2.0f));

    std::vector<Layer, arena_allocator<Layer>> layers(app->frameArena);
    layers.reserve(layerTextures.size());
    for (const auto& [layer, texture] : layerTextures) {
        layers.push_back(layer);
    }

    SDL_ReleaseGPUTexture(device, canvasViewTexture);
    const SDL_GPUTextureCreateInfo canvasTextureCreateInfo = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = textureFormat,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
               | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE,
        .width = static_cast<Uint32>(canvasView.width),
        .height = static_cast<Uint32>(canvasView.height),
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    canvasViewTexture = SDL_CreateGPUTexture(device, &canvasTextureCreateInfo);
    if (canvasViewTexture == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create canvas layer texture");
        return Error::Unknown;
    }

    for (auto layer : layers) {
        DestroyLayerTexture(canvasView, layer);
        CreateLayerTexture(canvasView, layer);
    }

    return Error::None;
}

bool Renderer::CanQuit() const {
    for (const auto& buf : tileDownloadStagingBuffers) {
        if (buf.state != StagingBufferState::None) {
            return false;
        }
    }

    return true;
}

void Renderer::Quit() {
    ZoneScoped;

    SDL_UnmapGPUTransferBuffer(device, paintStrokePointTransferBuffer);
    SDL_ReleaseGPUTexture(device, brushTexture);
    SDL_ReleaseGPUSampler(device, brushSampler);
    SDL_ReleaseGPUTransferBuffer(device, paintStrokePointTransferBuffer);
    SDL_ReleaseGPUBuffer(device, paintStrokePointBuffer);
    SDL_ReleaseGPUComputePipeline(device, eraseComputePipeline);
    SDL_ReleaseGPUComputePipeline(device, paintComputePipeline);

    SDL_ReleaseGPUComputePipeline(device, mergeComputePipeline);

    for (const auto& [tile, texture] : tileTextures) {
        SDL_ReleaseGPUTexture(device, texture);
    }

    for (auto& buf : tileDownloadStagingBuffers) {
        SDL_UnmapGPUTransferBuffer(device, buf.buffer);
        SDL_ReleaseGPUTransferBuffer(device, buf.buffer);
    }

    for (auto& buf : tileUploadStagingBuffers) {
        SDL_UnmapGPUTransferBuffer(device, buf.buffer);
        SDL_ReleaseGPUTransferBuffer(device, buf.buffer);
    }

    SDL_ReleaseGPUTransferBuffer(device, tileBlankTextureBuffer);
    SDL_ReleaseGPUSampler(device, tileSampler);
    SDL_ReleaseGPUGraphicsPipeline(device, tileGraphicsPipeline);

    for (const auto& [layer, texture] : layerTextures) {
        SDL_ReleaseGPUTexture(device, texture);
    }
    SDL_ReleaseGPUTexture(device, canvasViewTexture);
    SDL_ReleaseGPUSampler(device, layerSampler);
    SDL_ReleaseGPUGraphicsPipeline(device, layerGraphicsPipeline);

    SDL_ReleaseWindowFromGPUDevice(device, app->window);
    SDL_DestroyGPUDevice(device);

    device = nullptr;
}

bool Renderer::CreateLayerTexture(const CanvasView& view, const Layer layer) {
    ZoneScoped;
    SDL_assert(!layerTextures.contains(layer) && "Layer already has a gpu texture");
    SDL_assert(view.width > 0 && "Cannot size a layer texture to a zero width window");
    SDL_assert(view.height > 0 && "Cannot size a layer texture to a zero height window");

    const SDL_GPUTextureCreateInfo textureCreateInfo = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = textureFormat,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
        .width = static_cast<Uint32>(view.width),
        .height = static_cast<Uint32>(view.height),
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &textureCreateInfo);
    if (texture == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create layer texture");
        return false;
    }

    if (!layerTextures.insert(layer, texture)) {
        SDL_ReleaseGPUTexture(device, texture);
        SDL_assert(false && "Failed to insert layerTexture");
        return false;
    }
    return true;
}

void Renderer::DestroyLayerTexture(const CanvasView& view, const Layer layer) {
    ZoneScoped;
    SDL_assert(layerTextures.contains(layer) && "No gpu texture for this layer");
    SDL_ReleaseGPUTexture(device, layerTextures.at(layer));
    layerTextures.erase(layer);
}

Error Renderer::CreateUninitializedTileTexture(Tile tile) {
    ZoneScoped;
    SDL_assert(!tileTextures.contains(tile) && "Tile already has a gpu texture");

    const SDL_GPUTextureCreateInfo textureCreateInfo = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = textureFormat,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE,
        .width = static_cast<Uint32>(TILE_WIDTH),
        .height = static_cast<Uint32>(TILE_HEIGHT),
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &textureCreateInfo);
    if (texture == nullptr) {
        SDL_assert(false && "Failed to create texture");
        return Error::Unknown;
    } else {
        if (!tileTextures.insert(tile, texture)) {
            SDL_ReleaseGPUTexture(device, texture);
            SDL_assert(false && "Failed to insert tile texture");
            return Error::Unknown;
        }
    }

    return Error::None;
}

Error Renderer::CreateTileTexture(Tile tile) {
    ZoneScoped;
    if (auto res = CreateUninitializedTileTexture(tile); res != Error::None) {
        return res;
    }

    tileTexturesToInitialize.insert(tile);
    return Error::None;
}

void Renderer::DestroyTileTexture(Tile tile) {
    ZoneScoped;
    SDL_assert(tileTextures.contains(tile) && "Tile texture not found");

    auto* texture = tileTextures.at(tile);
    if (texture) {
        SDL_ReleaseGPUTexture(device, texture);
    }

    tileTextures.erase(tile);
}

Error Renderer::MergeTiles(const CanvasFile& canvasFile, std::span<Tile> srcTiles, std::span<Tile> dstTiles,
                           MergeInfo info) {
    ZoneScoped;
    SDL_assert(srcTiles.size() == dstTiles.size());

    if (srcTiles.empty()) {
        return Error::None;
    }

    for (const auto& tile : srcTiles) {
        SDL_assert(tileTextures.contains(tile));
        SDL_assert(tileTextures.at(tile));
    }
    for (const auto& tile : dstTiles) {
        SDL_assert(tileTextures.contains(tile));
        SDL_assert(tileTextures.at(tile));
    }

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    if (commandBuffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
        return Error::Unknown;
    }

    MergeRenderData mergeRenderData = {
        .srcBlendMode = static_cast<std::uint32_t>(info.blendMode),
        .srcOpacity = info.opacity,
    };
    constexpr auto TILE_SIZE = glm::vec2(TILE_WIDTH, TILE_HEIGHT);
    const glm::ivec2 mergeComputeInvocations = glm::ceil(TILE_SIZE / 32.0f);

    for (size_t i = 0; i < srcTiles.size(); i++) {
        // TODO use the whole array instead of doing a unique compute pass per merge
        const SDL_GPUStorageTextureReadWriteBinding mergeLayerBinding[1] = {{
            .texture = tileTextures.at(dstTiles[i]),
            .mip_level = 0,
            .layer = 0,
        }};
        SDL_GPUComputePass* mergeComputePass = SDL_BeginGPUComputePass(commandBuffer, mergeLayerBinding, 1, nullptr, 0);

        SDL_BindGPUComputePipeline(mergeComputePass, mergeComputePipeline);
        SDL_PushGPUComputeUniformData(commandBuffer, 0, &mergeRenderData, sizeof(MergeRenderData));

        const SDL_GPUTextureSamplerBinding samplers[] = {{
            .texture = tileTextures.at(srcTiles[i]),
            .sampler = tileSampler,
        }};
        SDL_BindGPUComputeSamplers(mergeComputePass, 0, samplers, 1);
        SDL_DispatchGPUCompute(mergeComputePass, mergeComputeInvocations.x, mergeComputeInvocations.y, 1);
        SDL_EndGPUComputePass(mergeComputePass);
    }

    if (!SDL_SubmitGPUCommandBuffer(commandBuffer)) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
        return Error::Unknown;
    }

    return Error::None;
}

bool Renderer::BeginCopyPass() {
    ZoneScoped;
    SDL_assert(!currentCommandBuffer && "A command buffer is already open");
    SDL_assert(!currentCopyPass && "A copy pass is already open");

    currentCommandBuffer = SDL_AcquireGPUCommandBuffer(device);
    if (!currentCommandBuffer) {
        return false;
    }
    currentCopyPass = SDL_BeginGPUCopyPass(currentCommandBuffer);

    return true;
}

SDL_GPUTexture* Renderer::DuplicateTileTexture(Tile tile) {
    ZoneScoped;
    SDL_assert(tileTextures.contains(tile) && "No gpu texture for this tile");
    SDL_assert(tileTextures.at(tile) && "Tile texture is null");
    return DuplicateTileTexture(tileTextures.at(tile));
}

SDL_GPUTexture* Renderer::DuplicateTileTexture(SDL_GPUTexture* texture) const {
    SDL_assert(currentCommandBuffer && "No command buffer open; call BeginCopyPass() first");
    SDL_assert(currentCopyPass && "No copy pass open; call BeginCopyPass() first");
    SDL_assert(texture && "Cannot duplicate a null texture");

    const SDL_GPUTextureCreateInfo textureCreateInfo = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = app->renderer.textureFormat,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE,
        .width = static_cast<Uint32>(TILE_WIDTH),
        .height = static_cast<Uint32>(TILE_HEIGHT),
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };

    SDL_GPUTexture* newTexture = SDL_CreateGPUTexture(device, &textureCreateInfo);
    if (texture == nullptr) {
        return nullptr;
    }

    const SDL_GPUTextureLocation sourceLoc = {
        .texture = texture,
        .mip_level = 0,
        .layer = 0,
        .x = 0,
        .y = 0,
        .z = 0,
    };
    const SDL_GPUTextureLocation destLoc = {
        .texture = newTexture,
        .mip_level = 0,
        .layer = 0,
        .x = 0,
        .y = 0,
        .z = 0,
    };
    SDL_CopyGPUTextureToTexture(currentCopyPass, &sourceLoc, &destLoc, TILE_WIDTH, TILE_HEIGHT, 1, false);

    return newTexture;
}

bool Renderer::EndCopyPass(bool wait) {
    ZoneScoped;
    SDL_assert(currentCommandBuffer && "No command buffer open; call BeginCopyPass() first");
    SDL_assert(currentCopyPass && "No copy pass open; call BeginCopyPass() first");

    SDL_EndGPUCopyPass(currentCopyPass);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(currentCommandBuffer);
    if (!fence) {
        return false;
    }
    currentCopyPass = nullptr;
    currentCommandBuffer = nullptr;
    if (wait) {
        SDL_WaitForGPUFences(device, true, &fence, 1);
        SDL_ReleaseGPUFence(device, fence);
    }

    return true;
}

void Renderer::CancelCopyPass() {
    ZoneScoped;
    SDL_assert(currentCommandBuffer && "No command buffer open; call BeginCopyPass() first");
    SDL_assert(currentCopyPass && "No copy pass open; call BeginCopyPass() first");

    SDL_CancelGPUCommandBuffer(currentCommandBuffer);
    currentCopyPass = nullptr;
    currentCommandBuffer = nullptr;
}

StrokePoint StrokePoint::Lerp(StrokePoint start, StrokePoint end, float t) {
    start.position = glm::mix(start.position, end.position, t);
    start.color = glm::mix(start.color, end.color, t);
    start.flow = std::lerp(start.flow, end.flow, t);
    start.radius = std::lerp(start.radius, end.radius, t);
    start.hardness = std::lerp(start.hardness, end.hardness, t);
    return start;
}
} // namespace Midori
