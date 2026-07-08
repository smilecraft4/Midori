#include "renderer.h"

#include "SDL3/SDL_stdinc.h"
#include "app.h"
#include "canvas.h"
#include "layers.h"
#include "tiles.h"
#include <SDL3/SDL_assert.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_log.h>
#include <algorithm>
#include <backends/imgui_impl_sdlgpu3.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include "../deps/qoi/qoi.h"
#include <tracy/Tracy.hpp>

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
    layerVertexShader = SDL_CreateGPUShader(device, &vertexShaderCreateInfo);
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
    layerFragmentShader = SDL_CreateGPUShader(device, &fragmentShaderCreateInfo);
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
        .width = (Uint32)app->windowSize.x,
        .height = (Uint32)app->windowSize.y,
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    canvasTexture = SDL_CreateGPUTexture(device, &canvasTextureCreateInfo);
    if (canvasTexture == nullptr) {
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
    tileVertexShader = SDL_CreateGPUShader(device, &vertexShaderCreateInfo);
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
    tileFragmentShader = SDL_CreateGPUShader(device, &fragmentShaderCreateInfo);
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
        .size = TILE_MAX_UPLOAD_TRANSFER * TILE_WIDTH * TILE_HEIGHT * 4,
    };
    tileUploadBuffer = SDL_CreateGPUTransferBuffer(device, &uploadBufferCreateInfo);
    if (tileUploadBuffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create tile upload buffer: %s", SDL_GetError());
        return false;
    }

    tileUploadBufferPtr = (std::uint8_t*)SDL_MapGPUTransferBuffer(device, tileUploadBuffer, false);
    if (tileUploadBufferPtr == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to map tile upload transfer buffer: %s", SDL_GetError());
        return false;
    }
    freeTileUploadOffset.reserve(TILE_MAX_UPLOAD_TRANSFER);
    for (size_t i = 0; i < TILE_MAX_UPLOAD_TRANSFER; i++) {
        freeTileUploadOffset.push_back(i * TILE_WIDTH * TILE_HEIGHT * 4);
    }

    const SDL_GPUTransferBufferCreateInfo downloadBufferCreateInfo = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        .size = TILE_MAX_DOWNLOAD_TRANSFER * TILE_WIDTH * TILE_HEIGHT * 4,
    };
    tileDownloadBuffer = SDL_CreateGPUTransferBuffer(device, &downloadBufferCreateInfo);
    if (tileDownloadBuffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create tile download buffer: %s", SDL_GetError());
        return false;
    }
    tileDownloadBufferPtr = (std::uint8_t*)SDL_MapGPUTransferBuffer(device, tileDownloadBuffer, false);
    if (tileDownloadBufferPtr == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to map tile download transfer buffer: %s", SDL_GetError());
        return false;
    }
    freeTileDownloadOffset.reserve(TILE_MAX_DOWNLOAD_TRANSFER);
    for (size_t i = 0; i < TILE_MAX_DOWNLOAD_TRANSFER; i++) {
        freeTileDownloadOffset.push_back(i * TILE_WIDTH * TILE_HEIGHT * 4);
    }

    const SDL_GPUTransferBufferCreateInfo tileBlankTextureBufferCreateInfo = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = TILE_WIDTH * TILE_HEIGHT * 4,
    };
    tileBlankTextureBuffer = SDL_CreateGPUTransferBuffer(device, &tileBlankTextureBufferCreateInfo);
    if (tileBlankTextureBuffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create tile upload buffer: %s", SDL_GetError());
        return false;
    }

    auto* buf = (uint8_t*)SDL_MapGPUTransferBuffer(device, tileBlankTextureBuffer, false);
    memset(buf, 0, TILE_WIDTH * TILE_HEIGHT * 4);
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
        .size = MAX_PAINT_STROKE_POINTS * sizeof(Canvas::StrokePoint),
    };
    paintStrokePointBuffer = SDL_CreateGPUBuffer(device, &bufferCreateInfo);
    if (paintStrokePointBuffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create paint stroke buffer: %s", SDL_GetError());
        return false;
    }

    const SDL_GPUTransferBufferCreateInfo uploadBufferCreateInfo = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = MAX_PAINT_STROKE_POINTS * sizeof(Canvas::StrokePoint),
    };
    paintStrokePointTransferBuffer = SDL_CreateGPUTransferBuffer(device, &uploadBufferCreateInfo);
    if (paintStrokePointTransferBuffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create paint stroke upload buffer: %s", SDL_GetError());
        return false;
    }

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
        .width = (Uint32)desc.width,
        .height = (Uint32)desc.height,
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

    auto* buf = (uint8_t*)SDL_MapGPUTransferBuffer(device, brushTransferBuffer, false);
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
} // namespace Midori

bool Renderer::Render() {
    ZoneScoped;
    lastRenderedTilesNum = 0;
    lastLayerRenderedNum = 0;
    SDL_GPUCommandBuffer* commandBuffer = nullptr;
    SDL_GPUTexture* swapchainTexture = nullptr;

    // Initializing undefined tiles
    if (!tileTextureUninitialized.empty()) {
        ZoneScopedN("Initialize uninitialized textures");
        { // Acquire GPU command buffer
            ZoneScopedN("Acquire GPU command buffer");
            commandBuffer = SDL_AcquireGPUCommandBuffer(device);
            if (commandBuffer == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
                return false;
            }
        }

        SDL_GPUCopyPass* uploadPass = SDL_BeginGPUCopyPass(commandBuffer);
        for (const auto& tile : tileTextureUninitialized) {
            const SDL_GPUTextureTransferInfo transferInfo = {
                .transfer_buffer = tileBlankTextureBuffer,
                .pixels_per_row = TILE_WIDTH,
                .rows_per_layer = TILE_HEIGHT,
            };
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
            SDL_UploadToGPUTexture(uploadPass, &transferInfo, &textureRegion, false);
        }
        SDL_EndGPUCopyPass(uploadPass);
        tileTextureUninitialized.clear();

        {
            ZoneScopedN("Submiting GPU command buffer");
            if (!SDL_SubmitGPUCommandBuffer(commandBuffer)) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
                return false;
            }
        }
    }

    // Uploading Tiles
    if (!allocatedTileUploadOffset.empty()) {
        SDL_UnmapGPUTransferBuffer(device, tileUploadBuffer);

        { // Acquire GPU command buffer
            ZoneScopedN("Acquire GPU command buffer");
            commandBuffer = SDL_AcquireGPUCommandBuffer(device);
            if (commandBuffer == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
                return false;
            }
        }

        ZoneScopedN("Uploading Tiles");
        SDL_GPUCopyPass* uploadPass = SDL_BeginGPUCopyPass(commandBuffer);

        for (const auto& [tile, offset] : allocatedTileUploadOffset) {
            ZoneScopedN("Uploading Tile");
            if (!tileTextures.contains(tile)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Tile not found, discard tile gpu upload");
                freeTileUploadOffset.push_back(offset);
                continue;
            }

            const SDL_GPUTextureTransferInfo transferInfo = {
                .transfer_buffer = tileUploadBuffer,
                .offset = (Uint32)offset,
                .pixels_per_row = TILE_WIDTH,
                .rows_per_layer = TILE_HEIGHT,
            };
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
            SDL_UploadToGPUTexture(uploadPass, &transferInfo, &textureRegion, false);
            freeTileUploadOffset.push_back(offset);
        }

        allocatedTileUploadOffset.clear();
        SDL_EndGPUCopyPass(uploadPass);

        {
            ZoneScopedN("Submiting GPU command buffer");
            if (!SDL_SubmitGPUCommandBuffer(commandBuffer)) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
                return false;
            }
        }

        tileUploadBufferPtr = (std::uint8_t*)SDL_MapGPUTransferBuffer(device, tileUploadBuffer, false);
    }

    // Download Tiles
    if (!allocatedTileDownloadOffset.empty()) {
        SDL_UnmapGPUTransferBuffer(device, tileDownloadBuffer);

        { // Acquire GPU command buffer
            ZoneScopedN("Acquire GPU command buffer");
            commandBuffer = SDL_AcquireGPUCommandBuffer(device);
            if (commandBuffer == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
                return false;
            }
        }

        ZoneScopedN("Downloading Tiles");
        SDL_GPUCopyPass* downloadPass = SDL_BeginGPUCopyPass(commandBuffer);

        for (const auto& [tile, offset] : allocatedTileDownloadOffset) {
            ZoneScopedN("Downloading Tile");
            SDL_assert(tileTextures.contains(tile) && "Downloading missing texture");

            const SDL_GPUTextureTransferInfo destination = {
                .transfer_buffer = tileDownloadBuffer,
                .offset = (Uint32)offset,
                .pixels_per_row = TILE_WIDTH,
                .rows_per_layer = TILE_HEIGHT,
            };
            const SDL_GPUTextureRegion source = {
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
            SDL_DownloadFromGPUTexture(downloadPass, &source, &destination);
        }

        SDL_EndGPUCopyPass(downloadPass);

        {
            ZoneScopedN("Submiting GPU command buffer");
            tileDownloadFence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
            if (tileDownloadFence == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
                return false;
            }
        }

        {
            ZoneScopedN("Waiting for texture download");
            // This can be a silent trigger
            SDL_WaitForGPUFences(device, true, &tileDownloadFence, 1);
            SDL_ReleaseGPUFence(device, tileDownloadFence);
            tileDownloadFence = nullptr;
            tileDownloadBufferPtr = (std::uint8_t*)SDL_MapGPUTransferBuffer(device, tileDownloadBuffer, false);
            for (const auto [tile, offset] : allocatedTileDownloadOffset) {
                tileDownloaded.insert(tile);
            }
        }
    }

    // Start painting the tiles
    if (!app->canvas.strokePoints.empty() && app->canvas.strokeStarted) {
        paintStrokePointTransferBufferPtr =
            (std::uint8_t*)SDL_MapGPUTransferBuffer(device, paintStrokePointTransferBuffer, false);
        memset(paintStrokePointTransferBufferPtr, 0, MAX_PAINT_STROKE_POINTS * sizeof(Canvas::StrokePoint));
        memcpy(paintStrokePointTransferBufferPtr, app->canvas.strokePoints.data(),
               app->canvas.strokePoints.size() * sizeof(Canvas::StrokePoint));
        SDL_UnmapGPUTransferBuffer(device, paintStrokePointTransferBuffer);

        { // Acquire GPU command buffer
            ZoneScopedN("Acquire GPU command buffer");
            commandBuffer = SDL_AcquireGPUCommandBuffer(device);
            if (commandBuffer == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
                return false;
            }
        }

        ZoneScopedN("Painting Tiles");

        // Copying data to the storage buffer
        SDL_GPUCopyPass* strokeCopyPass = SDL_BeginGPUCopyPass(commandBuffer);

        const SDL_GPUTransferBufferLocation source = {
            .transfer_buffer = paintStrokePointTransferBuffer,
            .offset = 0,
        };
        const SDL_GPUBufferRegion destination = {
            .buffer = paintStrokePointBuffer,
            .offset = 0,
            .size = static_cast<Uint32>(app->canvas.strokePoints.size() * sizeof(Canvas::StrokePoint)),
            // .size = MAX_PAINT_STROKE_POINTS * sizeof(Canvas::StrokePoint),
        };

        SDL_UploadToGPUBuffer(strokeCopyPass, &source, &destination, true);
        SDL_EndGPUCopyPass(strokeCopyPass);

        const StrokeRenderData strokeRenderData = {
            .pointsNum = static_cast<std::uint32_t>(app->canvas.strokePoints.size()),
        };
        SDL_PushGPUComputeUniformData(commandBuffer, 0, &strokeRenderData, sizeof(StrokeRenderData));
        const glm::ivec2 paintComputeInvocations = glm::ceil(glm::vec2(TILE_WIDTH / 32.0f, TILE_HEIGHT / 32.0f));
        for (const auto& tile : app->canvas.strokeTileAffected) {
            if (!tileTextures.contains(tile)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to get tile texture to paint");
                continue;
            }
            if (!app->canvas.tileInfos.contains(tile)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to get tile info to paint");
                continue;
            }
            const SDL_GPUStorageTextureReadWriteBinding paintTileBinding[1] = {{
                .texture = tileTextures.at(tile),
                .mip_level = 0,
                .layer = 0,
            }};
            if (app->canvas.brushMode) {
                SDL_GPUComputePass* paintComputePass =
                    SDL_BeginGPUComputePass(commandBuffer, paintTileBinding, 1, nullptr, 0);
                SDL_BindGPUComputePipeline(paintComputePass, paintComputePipeline);

                tileRenderData.position = app->canvas.tileInfos.at(tile).pos;
                tileRenderData.size = glm::vec2(TILE_WIDTH, TILE_WIDTH);
                SDL_PushGPUComputeUniformData(commandBuffer, 1, &tileRenderData, sizeof(TileRenderData));

                SDL_GPUTextureSamplerBinding samplerBinding = {
                    .texture = brushTexture,
                    .sampler = brushSampler,
                };
                SDL_BindGPUComputeSamplers(paintComputePass, 0, &samplerBinding, 1);

                SDL_BindGPUComputeStorageBuffers(paintComputePass, 0, &paintStrokePointBuffer, 1);

                SDL_DispatchGPUCompute(paintComputePass, paintComputeInvocations.x, paintComputeInvocations.y, 1);

                SDL_EndGPUComputePass(paintComputePass);

            } else if (app->canvas.eraserMode) {
                SDL_GPUComputePass* eraseComputePass =
                    SDL_BeginGPUComputePass(commandBuffer, paintTileBinding, 1, nullptr, 0);
                SDL_BindGPUComputePipeline(eraseComputePass, eraseComputePipeline);

                tileRenderData.position = app->canvas.tileInfos.at(tile).pos;
                tileRenderData.size = glm::vec2(TILE_WIDTH, TILE_HEIGHT);
                SDL_PushGPUComputeUniformData(commandBuffer, 1, &tileRenderData, sizeof(TileRenderData));

                SDL_GPUTextureSamplerBinding samplerBinding = {
                    .texture = brushTexture,
                    .sampler = brushSampler,
                };
                SDL_BindGPUComputeSamplers(eraseComputePass, 0, &samplerBinding, 1);

                SDL_BindGPUComputeStorageBuffers(eraseComputePass, 0, &paintStrokePointBuffer, 1);

                SDL_DispatchGPUCompute(eraseComputePass, paintComputeInvocations.x, paintComputeInvocations.y, 1);

                SDL_EndGPUComputePass(eraseComputePass);
            }
        }
        app->canvas.strokeTileAffected.clear();
        app->canvas.strokePoints.clear();

        {
            ZoneScopedN("Submiting GPU command buffer");
            auto* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
            if (fence == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
                return false;
            }
            SDL_WaitForGPUFences(device, true, &fence, 1);
            SDL_ReleaseGPUFence(device, fence);
        }
    }

    if (app->windowSize.x > 0 && app->windowSize.y > 0 && !app->hidden) {
        viewportRenderData.view = app->canvas.viewport.ViewMatrix();

        { // Acquire GPU command buffer
            ZoneScopedN("Acquire GPU command buffer");
            commandBuffer = SDL_AcquireGPUCommandBuffer(device);
            if (commandBuffer == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
                return false;
            }
        }

        // Get all layers that needs redrawing
        std::vector<LayerInfo> layerRendering;
        {
            ZoneScopedN("Layer Culling");
            {
                ZoneScopedN("Filtering layers");
                layerRendering.reserve(app->canvas.layerInfos.size());
                for (const auto& [layer, info] : app->canvas.layerInfos) {
                    if (info.hidden || info.opacity == 0.0f) {
                        continue;
                    }
                    if (app->canvas.layerToDelete.contains(layer)) {
                        continue;
                    }

                    SDL_assert(layerTextures.contains(layer) && "Layer texture not found");
                    layerRendering.push_back(info);
                }
            }
            {
                ZoneScopedN("Sorting layer by depth");
                // TODO: use app->canvas.layersHeightSorted instead
                std::ranges::sort(layerRendering, [](LayerInfo& a, LayerInfo& b) { return a.height > b.height; });
            }
        }

        { // Tile Rendering
            ZoneScopedN("Rendering Tile");
            for (const auto& layerInfo : layerRendering) {
                ZoneScopedN("Render Tile");
                // TODO: only redraw changed & visible tiles
                const SDL_GPUColorTargetInfo targetInfo = {
                    .texture = layerTextures[layerInfo.id],
                    .clear_color = SDL_FColor{.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f},
                    .load_op = SDL_GPU_LOADOP_CLEAR,
                    .store_op = SDL_GPU_STOREOP_STORE,
                };
                SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(commandBuffer, &targetInfo, 1, nullptr);
                SDL_BindGPUGraphicsPipeline(renderPass, tileGraphicsPipeline);

                SDL_PushGPUVertexUniformData(commandBuffer, 0, &viewportRenderData, sizeof(ViewportRenderData));

                for (const auto& tile : app->canvas.layerTiles[layerInfo.id]) {
                    if (app->canvas.tileToDelete.contains(tile) || app->canvas.tileToUnload.contains(tile)) {
                        continue;
                    }

                    const TileCoord tileInfo = app->canvas.tileInfos.at(tile);
                    tileRenderData.position = tileInfo.pos;
                    tileRenderData.size = glm::vec2(TILE_WIDTH, TILE_HEIGHT);
                    SDL_PushGPUVertexUniformData(commandBuffer, 1, &tileRenderData, sizeof(TileRenderData));

                    const SDL_GPUTextureSamplerBinding samplers[] = {{
                        .texture = tileTextures.at(tile),
                        .sampler = tileSampler,
                    }};
                    SDL_BindGPUFragmentSamplers(renderPass, 0, samplers, 1);

                    SDL_DrawGPUPrimitives(renderPass, 4, 1, 0, 0);
                    lastRenderedTilesNum++;
                }

                SDL_EndGPURenderPass(renderPass);
            }
        }

        { // Layer rendering
            ZoneScopedN("Layer blending and rendering");

            auto rgb = glm::vec3(app->bgColor);
            rgb = glm::mix(glm::pow((rgb + glm::vec3(0.055f)) * glm::vec3(1.0f / 1.055f), glm::vec3(2.4f)),
                           rgb * glm::vec3(1.0f / 12.92f), glm::lessThanEqual(rgb, glm::vec3(0.04045f)));
            rgb *= app->bgColor.a;

            const SDL_GPUColorTargetInfo targetInfo = {
                .texture = canvasTexture,
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
                .srcPos = glm::vec2(0.0),
                .srcSize = app->windowSize,
                .dstBlendMode = static_cast<std::uint32_t>(BlendMode::Alpha),
                .dstOpacity = 1.0f,
                .dstPos = glm::vec2(0.0),
                .dstSize = app->windowSize,
            };
            const SDL_GPUStorageTextureReadWriteBinding mergeLayerBinding[1] = {{
                .texture = canvasTexture,
                .mip_level = 0,
                .layer = 0,
            }};
            SDL_GPUComputePass* mergeComputePass =
                SDL_BeginGPUComputePass(commandBuffer, mergeLayerBinding, 1, nullptr, 0);
            if (mergeComputePass == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to create merge compute pass: %s", SDL_GetError());
                return false;
            }
            SDL_BindGPUComputePipeline(mergeComputePass, mergeComputePipeline);

            for (const auto& layerInfo : layerRendering) {
                ZoneScopedN("Blend layer to canvas");
                mergeRenderData.srcBlendMode = static_cast<std::uint32_t>(layerInfo.blendMode);
                mergeRenderData.srcOpacity = layerInfo.opacity;
                // Maybe dividing the buffer into a dst and src could be benificial
                SDL_PushGPUComputeUniformData(commandBuffer, 0, &mergeRenderData, sizeof(MergeRenderData));

                const SDL_GPUTextureSamplerBinding samplers[] = {{
                    .texture = layerTextures[layerInfo.id],
                    .sampler = layerSampler,
                }};
                SDL_BindGPUComputeSamplers(mergeComputePass, 0, samplers, 1);
                SDL_DispatchGPUCompute(mergeComputePass, mergeComputeInvocations.x, mergeComputeInvocations.y, 1);
            }

            SDL_EndGPUComputePass(mergeComputePass);
        }

        { // Wait and acquire GPU swapchain texture
            ZoneScopedN("Wait and acquire GPU swapchain texture");
            SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, app->window, &swapchainTexture, nullptr, nullptr);
            if (swapchainTexture == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire swapchain texture: %s", SDL_GetError());
                return false;
            }
        }
        {
            ZoneScopedN("Render canvas texture");
            const SDL_GPUColorTargetInfo targetInfo = {
                .texture = swapchainTexture,
                .clear_color = SDL_FColor{.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f},
                .load_op = SDL_GPU_LOADOP_CLEAR,
                .store_op = SDL_GPU_STOREOP_STORE,
            };
            SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(commandBuffer, &targetInfo, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(renderPass, layerGraphicsPipeline);

            const SDL_GPUTextureSamplerBinding samplers[] = {{
                .texture = canvasTexture,
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
                return false;
            }
            SDL_WaitForGPUFences(device, true, &fence, 1);
            SDL_ReleaseGPUFence(device, fence);
        }
    }

    return true;
}

bool Renderer::Resize() {
    ZoneScoped;
    viewportRenderData.projection =
        glm::ortho(-std::floor((float)app->windowSize.x / 2.0f), std::ceil((float)app->windowSize.x / 2.0f),
                   -std::floor((float)app->windowSize.y / 2.0f), std::ceil((float)app->windowSize.y / 2.0f));

    std::vector<Layer> layers;
    layers.reserve(layerTextures.size());
    for (const auto& [layer, texture] : layerTextures) {
        layers.push_back(layer);
    }

    SDL_ReleaseGPUTexture(device, canvasTexture);
    const SDL_GPUTextureCreateInfo canvasTextureCreateInfo = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = textureFormat,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET
               | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE,
        .width = (Uint32)app->windowSize.x,
        .height = (Uint32)app->windowSize.y,
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    canvasTexture = SDL_CreateGPUTexture(device, &canvasTextureCreateInfo);
    if (canvasTexture == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create canvas layer texture");
        return false;
    }

    for (auto layer : layers) {
        DeleteLayerTexture(layer);
        CreateLayerTexture(layer);
    }

    return true;
}

bool Renderer::CanQuit() {
    return allocatedTileDownloadOffset.empty();
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
    SDL_UnmapGPUTransferBuffer(device, tileDownloadBuffer);
    SDL_ReleaseGPUTransferBuffer(device, tileDownloadBuffer);
    SDL_UnmapGPUTransferBuffer(device, tileUploadBuffer);
    SDL_ReleaseGPUTransferBuffer(device, tileUploadBuffer);
    SDL_ReleaseGPUTransferBuffer(device, tileBlankTextureBuffer);
    SDL_ReleaseGPUSampler(device, tileSampler);
    SDL_ReleaseGPUGraphicsPipeline(device, tileGraphicsPipeline);
    SDL_ReleaseGPUShader(device, tileVertexShader);
    SDL_ReleaseGPUShader(device, tileFragmentShader);

    for (const auto& [layer, texture] : layerTextures) {
        SDL_ReleaseGPUTexture(device, texture);
    }
    SDL_ReleaseGPUTexture(device, canvasTexture);
    SDL_ReleaseGPUSampler(device, layerSampler);
    SDL_ReleaseGPUGraphicsPipeline(device, layerGraphicsPipeline);
    SDL_ReleaseGPUShader(device, layerVertexShader);
    SDL_ReleaseGPUShader(device, layerFragmentShader);

    SDL_ReleaseWindowFromGPUDevice(device, app->window);
    SDL_DestroyGPUDevice(device);

    device = nullptr;
}

bool Renderer::CreateLayerTexture(const Layer layer) {
    ZoneScoped;
    SDL_assert(!layerTextures.contains(layer));
    SDL_assert(app->windowSize.x > 0);
    SDL_assert(app->windowSize.y > 0);

    const SDL_GPUTextureCreateInfo textureCreateInfo = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = textureFormat,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
        .width = (Uint32)app->windowSize.x,
        .height = (Uint32)app->windowSize.y,
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &textureCreateInfo);
    if (texture == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create layer texture");
        return false;
    }

    layerTextures[layer] = texture;
    return true;
}

void Renderer::DeleteLayerTexture(const Layer layer) {
    ZoneScoped;
    SDL_assert(layerTextures.contains(layer));
    SDL_ReleaseGPUTexture(device, layerTextures.at(layer));
    layerTextures.erase(layer);
}

Renderer::TileTextureError Renderer::CreateTileTexture(const Tile tile) {
    ZoneScoped;
    SDL_assert(!tileTextures.contains(tile));

    const SDL_GPUTextureCreateInfo textureCreateInfo = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = textureFormat,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE,
        .width = (Uint32)TILE_WIDTH,
        .height = (Uint32)TILE_HEIGHT,
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &textureCreateInfo);
    if (texture == nullptr) {
        return TileTextureError::Unknwon;
    } else {
        tileTextures[tile] = texture;
        tileTextureUninitialized.insert(tile);
    }

    return TileTextureError::None;
}

Renderer::TileTextureError Renderer::UploadTileTexture(const Tile tile, const std::vector<uint8_t>& pixels) {
    ZoneScoped;
    if (!tileTextures.contains(tile)) {
        return TileTextureError::MissingTexture;
    }

    if (freeTileUploadOffset.empty()) {
        return TileTextureError::UploadSlotMissing;
    }

    if (!allocatedTileUploadOffset.contains(tile)) {
        size_t offset = freeTileUploadOffset.back();
        SDL_assert(offset <= (TILE_MAX_UPLOAD_TRANSFER - 1) * TILE_WIDTH * TILE_HEIGHT * 4);
        freeTileUploadOffset.pop_back();
        allocatedTileUploadOffset[tile] = offset;
    }

    auto* dst = (uint8_t*)(tileUploadBufferPtr + allocatedTileUploadOffset.at(tile));
    memcpy(dst, pixels.data(), pixels.size() * sizeof(pixels[0]));

    return TileTextureError::None;
}

void Renderer::ReleaseTileTexture(const Tile tile) {
    ZoneScoped;
    SDL_assert(tileTextures.contains(tile) && "Tile does not exists");
    SDL_ReleaseGPUTexture(device, tileTextures.at(tile));
    tileTextures.erase(tile);
}

// TODO: transform into a single command buffer action, MergeTileTextures(eastl::vector<std::pair<Tile, Tile>>
// tileToMerges)
bool Renderer::MergeTileTextures(const Tile overTile, const Tile belowTile) {
    ZoneScoped;
    SDL_assert(tileTextures.contains(overTile));
    SDL_assert(tileTextures.contains(belowTile));

    const auto overTileInfo = app->canvas.tileInfos.at(overTile);
    const auto belowTileInfo = app->canvas.tileInfos.at(belowTile);

    const auto overLayerInfo = app->canvas.layerInfos.at(overTileInfo.layer);
    const auto belowLayerInfo = app->canvas.layerInfos.at(belowTileInfo.layer);

    SDL_GPUCommandBuffer* commandBuffer = nullptr;
    { // Acquire GPU command buffer
        ZoneScopedN("Acquire GPU command buffer");
        commandBuffer = SDL_AcquireGPUCommandBuffer(device);
        if (commandBuffer == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
            return false;
        }
    }

    { // Layer rendering
        constexpr auto TILE_SIZE = glm::vec2(TILE_WIDTH, TILE_HEIGHT);
        ZoneScopedN("Layer blending and rendering");
        const glm::ivec2 mergeComputeInvocations = glm::ceil(TILE_SIZE / 32.0f);
        MergeRenderData mergeRenderData = {
            .srcBlendMode = static_cast<std::uint32_t>(overLayerInfo.blendMode),
            .srcOpacity = overLayerInfo.opacity,
            .srcPos = glm::vec2(0.0),
            .srcSize = TILE_SIZE,
            .dstBlendMode = static_cast<std::uint32_t>(belowLayerInfo.blendMode),
            .dstOpacity = belowLayerInfo.opacity,
            .dstPos = glm::vec2(0.0),
            .dstSize = TILE_SIZE,
        };
        const SDL_GPUStorageTextureReadWriteBinding mergeLayerBinding[1] = {{
            .texture = tileTextures.at(belowTile),
            .mip_level = 0,
            .layer = 0,
        }};
        SDL_GPUComputePass* mergeComputePass = SDL_BeginGPUComputePass(commandBuffer, mergeLayerBinding, 1, nullptr, 0);
        if (mergeComputePass == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to create merge compute pass: %s", SDL_GetError());
            return false;
        }
        SDL_BindGPUComputePipeline(mergeComputePass, mergeComputePipeline);
        SDL_PushGPUComputeUniformData(commandBuffer, 0, &mergeRenderData, sizeof(MergeRenderData));

        const SDL_GPUTextureSamplerBinding samplers[] = {{
            .texture = tileTextures.at(overTile),
            .sampler = tileSampler,
        }};
        SDL_BindGPUComputeSamplers(mergeComputePass, 0, samplers, 1);
        SDL_DispatchGPUCompute(mergeComputePass, mergeComputeInvocations.x, mergeComputeInvocations.y, 1);

        SDL_EndGPUComputePass(mergeComputePass);
    }

    {
        ZoneScopedN("Submiting GPU command buffer");
        if (!SDL_SubmitGPUCommandBuffer(commandBuffer)) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
            return false;
        }
    }

    return true;
}

bool Renderer::DownloadTileTexture(Tile tile) {
    ZoneScoped;
    SDL_assert(tile > 0 && "Tile is invalid");
    SDL_assert(tileTextures.contains(tile));
    SDL_assert(!allocatedTileDownloadOffset.contains(tile) && "??");
    SDL_assert(!tileDownloaded.contains(tile));

    if (freeTileDownloadOffset.empty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No slot buffer slot for donwloading tile");
        return false;
    }

    const auto tileOffset = freeTileDownloadOffset.back();
    freeTileDownloadOffset.pop_back();
    SDL_assert(tileOffset <= (TILE_MAX_DOWNLOAD_TRANSFER - 1) * TILE_WIDTH * TILE_HEIGHT * 4);

    allocatedTileDownloadOffset[tile] = tileOffset;

    return true;
}

bool Renderer::IsTileTextureDownloaded(Tile tile) const {
    ZoneScoped;
    SDL_assert(tile > 0 && "Tile is invalid");
    SDL_assert(allocatedTileDownloadOffset.contains(tile) && "Tile not allocated");

    return tileDownloaded.contains(tile);
}

bool Renderer::CopyTileTextureDownloaded(const Tile tile, std::vector<uint8_t>& tileTexture) {
    ZoneScoped;
    SDL_assert(tile > 0 && "Tile is invalid");
    SDL_assert(IsTileTextureDownloaded(tile));

    tileTexture.resize(TILE_WIDTH * TILE_HEIGHT * 4);

    SDL_assert(tileDownloadBufferPtr != nullptr);
    SDL_assert(allocatedTileDownloadOffset.at(tile) >= 0);
    SDL_assert(allocatedTileDownloadOffset.at(tile)
               <= (TILE_MAX_DOWNLOAD_TRANSFER - 1) * (TILE_WIDTH * TILE_HEIGHT * 4));
    memcpy(tileTexture.data(), tileDownloadBufferPtr + allocatedTileDownloadOffset.at(tile),
           (TILE_WIDTH * TILE_HEIGHT * 4));

    freeTileDownloadOffset.push_back(allocatedTileDownloadOffset.at(tile));
    tileDownloaded.erase(tile);
    allocatedTileDownloadOffset.erase(tile);

    return true;
}

SDL_GPUTexture* Renderer::DuplicateTileTexture(SDL_GPUCopyPass* copyPass, SDL_GPUTexture* tileTexture) const {
    ZoneScoped;

    const SDL_GPUTextureCreateInfo textureCreateInfo = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = app->renderer.textureFormat,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE,
        .width = (Uint32)TILE_WIDTH,
        .height = (Uint32)TILE_HEIGHT,
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(app->renderer.device, &textureCreateInfo);
    const SDL_GPUTextureLocation sourceLoc = {
        .texture = tileTexture, .mip_level = 0, .layer = 0, .x = 0, .y = 0, .z = 0};
    const SDL_GPUTextureLocation destLoc = {.texture = texture, .mip_level = 0, .layer = 0, .x = 0, .y = 0, .z = 0};
    SDL_CopyGPUTextureToTexture(copyPass, &sourceLoc, &destLoc, TILE_WIDTH, TILE_HEIGHT, 1, false);

    return texture;
}
} // namespace Midori
