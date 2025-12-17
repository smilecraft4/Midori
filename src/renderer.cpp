#include "midori/renderer.h"

#include <SDL3/SDL_assert.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_log.h>
#include <imgui.h>
#include <imgui_impl_sdlgpu3.h>
#include <qoi/qoi.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <tracy/Tracy.hpp>
#include <vector>

#include "midori/app.h"
#include "midori/canvas.h"
#include "midori/types.h"

namespace Midori {

Renderer::Renderer(App *app) : app(app) {}

bool Renderer::Init() {
    ZoneScoped;
    device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, "vulkan");
    if (device == nullptr) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to create gpu device: %s", SDL_GetError());
        return false;
    }

    if (!SDL_ClaimWindowForGPUDevice(device, app->window)) {
        SDL_LogCritical(SDL_LOG_CATEGORY_APPLICATION, "Failed to create gpu device: %s", SDL_GetError());
        return false;
    }
    SDL_SetGPUSwapchainParameters(device, app->window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

    swapchain_format = SDL_GetGPUSwapchainTextureFormat(device, app->window);

    viewport_render_data.projection =
        glm::ortho(-((float)app->window_size.x / 2.0f), ((float)app->window_size.x / 2.0f),
                   -((float)app->window_size.y / 2.0f), ((float)app->window_size.y / 2.0f));

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
    size_t vertex_code_size = 0;
    auto *vertex_code = (Uint8 *)SDL_LoadFile("./data/shaders/vulkan/layer.vert.spv", &vertex_code_size);
    if (vertex_code_size == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open file \"%s\": %s",
                     "./data/shaders/vulkan/layer.vert.spv", SDL_GetError());
        return false;
    }
    const SDL_GPUShaderCreateInfo vertex_shader_create_info = {
        .code_size = vertex_code_size,
        .code = vertex_code,
        .entrypoint = "main",
        .format = SDL_GPU_SHADERFORMAT_SPIRV,
        .stage = SDL_GPU_SHADERSTAGE_VERTEX,
        .num_samplers = 0,
        .num_storage_textures = 0,
        .num_storage_buffers = 0,
        .num_uniform_buffers = 1,
    };
    layer_vertex_shader = SDL_CreateGPUShader(device, &vertex_shader_create_info);
    if (layer_vertex_shader == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create layer vertex shader: %s", SDL_GetError());
    }

    size_t fragment_code_size = 0;
    auto *fragment_code = (Uint8 *)SDL_LoadFile("./data/shaders/vulkan/layer.frag.spv", &fragment_code_size);
    if (fragment_code_size == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open file \"%s\": %s",
                     "./data/shaders/vulkan/layer.frag.spv", SDL_GetError());
        return false;
    }
    const SDL_GPUShaderCreateInfo fragment_shader_create_info = {
        .code_size = fragment_code_size,
        .code = fragment_code,
        .entrypoint = "main",
        .format = SDL_GPU_SHADERFORMAT_SPIRV,
        .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
        .num_samplers = 1,
        .num_storage_textures = 0,
        .num_storage_buffers = 0,
        .num_uniform_buffers = 0,
    };
    layer_fragment_shader = SDL_CreateGPUShader(device, &fragment_shader_create_info);
    if (layer_fragment_shader == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create layer fragment shader: %s", SDL_GetError());
    }

    SDL_GPUColorTargetDescription color_target_descriptions[] = {{
        .format = swapchain_format,
        .blend_state =
            {
                .src_color_blendfactor = SDL_GPU_BLENDFACTOR_INVALID,
                .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_INVALID,
                .color_blend_op = SDL_GPU_BLENDOP_INVALID,
                .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_INVALID,
                .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_INVALID,
                .alpha_blend_op = SDL_GPU_BLENDOP_INVALID,
                .color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B |
                                    SDL_GPU_COLORCOMPONENT_A,
                .enable_blend = false,
                .enable_color_write_mask = false,
            },
    }};

    const SDL_GPUGraphicsPipelineCreateInfo pipeline_create_info = {
        .vertex_shader = layer_vertex_shader,
        .fragment_shader = layer_fragment_shader,
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
                .back_stencil_state = {SDL_GPU_STENCILOP_INVALID},
                .front_stencil_state = {SDL_GPU_STENCILOP_INVALID},
                .compare_mask = 0,
                .write_mask = 0,
                .enable_depth_test = false,
                .enable_depth_write = false,
                .enable_stencil_test = false,
            },
        .target_info =
            {
                .color_target_descriptions = color_target_descriptions,
                .num_color_targets = 1,
                .depth_stencil_format = SDL_GPU_TEXTUREFORMAT_INVALID,
                .has_depth_stencil_target = false,
            },

    };
    layer_graphics_pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipeline_create_info);
    if (layer_graphics_pipeline == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to create layer graphics pipeline: %s", SDL_GetError());
        return false;
    }

    const SDL_GPUSamplerCreateInfo sampler_create_info = {
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
    layer_sampler = SDL_CreateGPUSampler(device, &sampler_create_info);
    if (layer_sampler == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to create layer sampler: %s", SDL_GetError());
        return false;
    }

    const SDL_GPUTextureCreateInfo canvas_texture_create_info = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = texture_format,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                 SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE,
        .width = (Uint32)app->window_size.x,
        .height = (Uint32)app->window_size.y,
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    canvas_texture = SDL_CreateGPUTexture(device, &canvas_texture_create_info);
    if (canvas_texture == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to base canvas texture");
        return false;
    }

    return true;
}

bool Renderer::InitTiles() {
    ZoneScoped;
    size_t vertex_code_size = 0;
    auto *vertex_code = (Uint8 *)SDL_LoadFile("./data/shaders/vulkan/tile.vert.spv", &vertex_code_size);
    if (vertex_code_size == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open file \"%s\": %s",
                     "./data/shaders/vulkan/tile.vert.spv", SDL_GetError());
        return false;
    }
    const SDL_GPUShaderCreateInfo vertex_shader_create_info = {
        .code_size = vertex_code_size,
        .code = vertex_code,
        .entrypoint = "main",
        .format = SDL_GPU_SHADERFORMAT_SPIRV,
        .stage = SDL_GPU_SHADERSTAGE_VERTEX,
        .num_samplers = 0,
        .num_storage_textures = 0,
        .num_storage_buffers = 0,
        .num_uniform_buffers = 2,
    };
    tile_vertex_shader = SDL_CreateGPUShader(device, &vertex_shader_create_info);
    if (tile_vertex_shader == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create tile vertex shader: %s", SDL_GetError());
    }
    size_t fragment_code_size = 0;
    auto *fragment_code = (Uint8 *)SDL_LoadFile("./data/shaders/vulkan/tile.frag.spv", &fragment_code_size);
    if (fragment_code_size == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open file \"%s\": %s",
                     "./data/shaders/vulkan/tile.frag.spv", SDL_GetError());
        return false;
    }
    const SDL_GPUShaderCreateInfo fragment_shader_create_info = {
        .code_size = fragment_code_size,
        .code = fragment_code,
        .entrypoint = "main",
        .format = SDL_GPU_SHADERFORMAT_SPIRV,
        .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
        .num_samplers = 1,
        .num_storage_textures = 0,
        .num_storage_buffers = 0,
        .num_uniform_buffers = 0,
    };
    tile_fragment_shader = SDL_CreateGPUShader(device, &fragment_shader_create_info);
    if (tile_fragment_shader == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create tile fragment shader: %s", SDL_GetError());
    }

    SDL_GPUColorTargetDescription color_target_descriptions[] = {{
        .format = texture_format,
        .blend_state =
            {
                .src_color_blendfactor = SDL_GPU_BLENDFACTOR_INVALID,
                .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_INVALID,
                .color_blend_op = SDL_GPU_BLENDOP_INVALID,
                .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_INVALID,
                .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_INVALID,
                .alpha_blend_op = SDL_GPU_BLENDOP_INVALID,
                .color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B |
                                    SDL_GPU_COLORCOMPONENT_A,
                .enable_blend = false,
                .enable_color_write_mask = false,
            },
    }};

    const SDL_GPUGraphicsPipelineCreateInfo pipeline_create_info = {
        .vertex_shader = tile_vertex_shader,
        .fragment_shader = tile_fragment_shader,
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
                .back_stencil_state = {SDL_GPU_STENCILOP_INVALID},
                .front_stencil_state = {SDL_GPU_STENCILOP_INVALID},
                .compare_mask = 0,
                .write_mask = 0,
                .enable_depth_test = false,
                .enable_depth_write = false,
                .enable_stencil_test = false,
            },
        .target_info =
            {
                .color_target_descriptions = color_target_descriptions,
                .num_color_targets = 1,
                .depth_stencil_format = SDL_GPU_TEXTUREFORMAT_INVALID,
                .has_depth_stencil_target = false,
            },

    };
    tile_graphics_pipeline = SDL_CreateGPUGraphicsPipeline(device, &pipeline_create_info);
    if (tile_graphics_pipeline == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to create tile graphics pipeline: %s", SDL_GetError());
        return false;
    }

    const SDL_GPUSamplerCreateInfo sampler_create_info = {
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
    tile_sampler = SDL_CreateGPUSampler(device, &sampler_create_info);
    if (tile_sampler == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to create tile sampler: %s", SDL_GetError());
        return false;
    }

    const SDL_GPUTransferBufferCreateInfo upload_buffer_create_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = TILE_MAX_UPLOAD_TRANSFER * TILE_SIZE * TILE_SIZE * 4,
    };
    tile_upload_buffer = SDL_CreateGPUTransferBuffer(device, &upload_buffer_create_info);
    if (tile_upload_buffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create tile upload buffer: %s", SDL_GetError());
        return false;
    }

    tile_upload_buffer_ptr = (std::uint8_t *)SDL_MapGPUTransferBuffer(device, tile_upload_buffer, false);
    if (tile_upload_buffer_ptr == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to map tile upload transfer buffer: %s", SDL_GetError());
        return false;
    }
    free_tile_upload_offset.reserve(TILE_MAX_UPLOAD_TRANSFER);
    for (size_t i = 0; i < TILE_MAX_UPLOAD_TRANSFER; i++) {
        free_tile_upload_offset.push_back(i * TILE_SIZE * TILE_SIZE * 4);
    }

    const SDL_GPUTransferBufferCreateInfo download_buffer_create_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        .size = TILE_MAX_DOWNLOAD_TRANSFER * TILE_SIZE * TILE_SIZE * 4,
    };
    tile_download_buffer = SDL_CreateGPUTransferBuffer(device, &download_buffer_create_info);
    if (tile_download_buffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create tile download buffer: %s", SDL_GetError());
        return false;
    }
    tile_download_buffer_ptr = (std::uint8_t *)SDL_MapGPUTransferBuffer(device, tile_download_buffer, false);
    if (tile_download_buffer_ptr == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to map tile download transfer buffer: %s", SDL_GetError());
        return false;
    }
    free_tile_download_offset.reserve(TILE_MAX_DOWNLOAD_TRANSFER);
    for (size_t i = 0; i < TILE_MAX_DOWNLOAD_TRANSFER; i++) {
        free_tile_download_offset.push_back(i * TILE_SIZE * TILE_SIZE * 4);
    }

    const SDL_GPUTransferBufferCreateInfo tile_blank_texture_buffer_create_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = TILE_SIZE * TILE_SIZE * 4,
    };
    tile_blank_texture_buffer = SDL_CreateGPUTransferBuffer(device, &tile_blank_texture_buffer_create_info);
    if (tile_blank_texture_buffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create tile upload buffer: %s", SDL_GetError());
        return false;
    }

    auto *buf = (uint8_t *)SDL_MapGPUTransferBuffer(device, tile_blank_texture_buffer, false);
    memset(buf, 0, TILE_SIZE * TILE_SIZE * 4);
    SDL_UnmapGPUTransferBuffer(device, tile_blank_texture_buffer);

    return true;
}

bool Renderer::InitMerge() {
    size_t code_size = 0;
    auto *code = (Uint8 *)SDL_LoadFile("./data/shaders/vulkan/merge.comp.spv", &code_size);
    if (code_size == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open file \"%s\": %s",
                     "./data/shaders/vulkan/merge.comp.spv", SDL_GetError());
        return false;
    }
    const SDL_GPUComputePipelineCreateInfo create_info = {
        .code_size = code_size,
        .code = code,
        .entrypoint = "main",
        .format = SDL_GPU_SHADERFORMAT_SPIRV,
        .num_samplers = 1,
        .num_readonly_storage_textures = 0,  // src texture
        .num_readonly_storage_buffers = 0,
        .num_readwrite_storage_textures = 1,  // dst texture
        .num_readwrite_storage_buffers = 0,
        .num_uniform_buffers = 1,
        .threadcount_x = 32,
        .threadcount_y = 32,
        .threadcount_z = 1,
    };
    merge_compute_pipeline = SDL_CreateGPUComputePipeline(device, &create_info);
    if (merge_compute_pipeline == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize merge compute pipeline: %s", SDL_GetError());
        return false;
    }

    return true;
}

bool Renderer::InitPaint() {
    size_t paint_code_size = 0;
    auto *paint_code = (Uint8 *)SDL_LoadFile("./data/shaders/vulkan/paint.comp.spv", &paint_code_size);
    if (paint_code_size == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open file \"%s\": %s",
                     "./data/shaders/vulkan/paint.comp.spv", SDL_GetError());
        return false;
    }
    const SDL_GPUComputePipelineCreateInfo paint_create_info = {
        .code_size = paint_code_size,
        .code = paint_code,
        .entrypoint = "main",
        .format = SDL_GPU_SHADERFORMAT_SPIRV,
        .num_samplers = 1,  // alpha brush
        .num_readonly_storage_textures = 0,
        .num_readonly_storage_buffers = 1,    // stroke buffer
        .num_readwrite_storage_textures = 1,  // dst texture
        .num_readwrite_storage_buffers = 0,
        .num_uniform_buffers = 2,  // tile + stroke
        .threadcount_x = 32,
        .threadcount_y = 32,
        .threadcount_z = 1,
    };
    paint_compute_pipeline = SDL_CreateGPUComputePipeline(device, &paint_create_info);
    if (paint_compute_pipeline == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize paint compute pipeline: %s", SDL_GetError());
        return false;
    }
    SDL_free(paint_code);

    size_t erase_code_size = 0;
    auto *erase_code = (Uint8 *)SDL_LoadFile("./data/shaders/vulkan/erase.comp.spv", &erase_code_size);
    if (erase_code_size == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open file \"%s\": %s",
                     "./data/shaders/vulkan/erase.comp.spv", SDL_GetError());
        return false;
    }
    const SDL_GPUComputePipelineCreateInfo erase_create_info = {
        .code_size = erase_code_size,
        .code = erase_code,
        .entrypoint = "main",
        .format = SDL_GPU_SHADERFORMAT_SPIRV,
        .num_samplers = 1,  // alpha brush
        .num_readonly_storage_textures = 0,
        .num_readonly_storage_buffers = 1,    // stroke buffer
        .num_readwrite_storage_textures = 1,  // dst texture
        .num_readwrite_storage_buffers = 0,
        .num_uniform_buffers = 2,  // tile + stroke
        .threadcount_x = 32,
        .threadcount_y = 32,
        .threadcount_z = 1,
    };
    erase_compute_pipeline = SDL_CreateGPUComputePipeline(device, &erase_create_info);
    if (erase_compute_pipeline == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize erase compute pipeline: %s", SDL_GetError());
        return false;
    }
    SDL_free(erase_code);

    const SDL_GPUBufferCreateInfo buffer_create_info = {
        .usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
        .size = MAX_PAINT_STROKE_POINTS * sizeof(Canvas::StrokePoint),
    };
    paint_stroke_point_buffer = SDL_CreateGPUBuffer(device, &buffer_create_info);
    if (paint_stroke_point_buffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create paint stroke buffer: %s", SDL_GetError());
        return false;
    }

    const SDL_GPUTransferBufferCreateInfo upload_buffer_create_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = MAX_PAINT_STROKE_POINTS * sizeof(Canvas::StrokePoint),
    };
    paint_stroke_point_transfer_buffer = SDL_CreateGPUTransferBuffer(device, &upload_buffer_create_info);
    if (paint_stroke_point_transfer_buffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create paint stroke upload buffer: %s", SDL_GetError());
        return false;
    }

    const std::string path = SDL_GetPrefPath(nullptr, "midori");
    const std::string brushPath = std::format("{}brushes/sphere.qoi", path);

    size_t dataSize;
    auto *data = (uint8_t *)SDL_LoadFile(brushPath.c_str(), &dataSize);
    if (data == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read sphere brush texture: %s", SDL_GetError());
        return false;
    }

    qoi_desc desc;
    auto *pixels = (uint8_t *)qoi_decode(data, dataSize, &desc, 4);
    SDL_free(data);
    if (pixels == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to decode brush texture");
        return false;
    }

    // for (size_t i = 0; i < (size_t)desc.width * desc.height; i++) {
    //     pixels[i] = pixels[i * 3];
    // }

    SDL_GPUTextureCreateInfo brush_texture_create_info = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = texture_format,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
        .width = (Uint32)desc.width,
        .height = (Uint32)desc.height,
        .layer_count_or_depth = 1,
        .num_levels = 10,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    brush_texture = SDL_CreateGPUTexture(device, &brush_texture_create_info);
    if (brush_texture == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create brush texture: %s", SDL_GetError());
        return false;
    }

    SDL_GPUTransferBufferCreateInfo transferBufferCreateInfo = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size = desc.width * desc.height * 4,
    };
    SDL_GPUTransferBuffer *brushTransferBuffer = SDL_CreateGPUTransferBuffer(device, &transferBufferCreateInfo);
    if (brushTransferBuffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create temp brush transfer buffer: %s", SDL_GetError());
        return false;
    }

    auto *buf = (uint8_t *)SDL_MapGPUTransferBuffer(device, brushTransferBuffer, false);
    memcpy(buf, pixels, (size_t)desc.width * desc.height * 4);
    SDL_UnmapGPUTransferBuffer(device, brushTransferBuffer);
    free(pixels);

    SDL_GPUCommandBuffer *command_buffer = SDL_AcquireGPUCommandBuffer(device);
    if (command_buffer == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
        return false;
    }
    SDL_GPUCopyPass *upload_pass = SDL_BeginGPUCopyPass(command_buffer);
    const SDL_GPUTextureTransferInfo transfer_info = {
        .transfer_buffer = brushTransferBuffer,
        .pixels_per_row = desc.width,
        .rows_per_layer = desc.height,
    };
    const SDL_GPUTextureRegion texture_region = {
        .texture = brush_texture,
        .w = desc.width,
        .h = desc.height,
        .d = 1,
    };
    SDL_UploadToGPUTexture(upload_pass, &transfer_info, &texture_region, false);
    SDL_EndGPUCopyPass(upload_pass);
    SDL_GenerateMipmapsForGPUTexture(command_buffer, brush_texture);
    if (!SDL_SubmitGPUCommandBuffer(command_buffer)) {
        SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
        return false;
    }

    SDL_ReleaseGPUTransferBuffer(device, brushTransferBuffer);

    const SDL_GPUSamplerCreateInfo brush_sampler_create_info = {
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
    brush_sampler = SDL_CreateGPUSampler(device, &brush_sampler_create_info);
    if (brush_sampler == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create brush sampler: %s", SDL_GetError());
        return false;
    }

    return true;
}  // namespace Midori

bool Renderer::Render() {
    ZoneScoped;
    last_rendered_tiles_num = 0;
    last_layer_rendered_num = 0;
    SDL_GPUCommandBuffer *command_buffer = nullptr;
    SDL_GPUTexture *swapchain_texture = nullptr;

    // Initializing undefined tiles
    if (!tile_texture_uninitialized.empty()) {
        ZoneScopedN("Initialize uninitialized textures");
        {  // Acquire GPU command buffer
            ZoneScopedN("Acquire GPU command buffer");
            command_buffer = SDL_AcquireGPUCommandBuffer(device);
            if (command_buffer == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
                return false;
            }
        }

        SDL_GPUCopyPass *upload_pass = SDL_BeginGPUCopyPass(command_buffer);
        for (const auto &tile : tile_texture_uninitialized) {
            const SDL_GPUTextureTransferInfo transfer_info = {
                .transfer_buffer = tile_blank_texture_buffer,
                .pixels_per_row = TILE_SIZE,
                .rows_per_layer = TILE_SIZE,
            };
            const SDL_GPUTextureRegion texture_region = {
                .texture = tile_textures.at(tile),
                .mip_level = 0,
                .layer = 0,
                .x = 0,
                .y = 0,
                .z = 0,
                .w = TILE_SIZE,
                .h = TILE_SIZE,
                .d = 1,
            };
            SDL_UploadToGPUTexture(upload_pass, &transfer_info, &texture_region, false);
        }
        SDL_EndGPUCopyPass(upload_pass);
        tile_texture_uninitialized.clear();

        {
            ZoneScopedN("Submiting GPU command buffer");
            if (!SDL_SubmitGPUCommandBuffer(command_buffer)) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
                return false;
            }
        }
    }

    // Uploading Tiles
    if (!allocated_tile_upload_offset.empty()) {
        SDL_UnmapGPUTransferBuffer(device, tile_upload_buffer);

        {  // Acquire GPU command buffer
            ZoneScopedN("Acquire GPU command buffer");
            command_buffer = SDL_AcquireGPUCommandBuffer(device);
            if (command_buffer == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
                return false;
            }
        }

        ZoneScopedN("Uploading Tiles");
        SDL_GPUCopyPass *upload_pass = SDL_BeginGPUCopyPass(command_buffer);

        for (const auto &[tile, offset] : allocated_tile_upload_offset) {
            ZoneScopedN("Uploading Tile");
            if (!tile_textures.contains(tile)) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Tile not found, discard tile gpu upload");
                free_tile_upload_offset.push_back(offset);
                continue;
            }

            const SDL_GPUTextureTransferInfo transfer_info = {
                .transfer_buffer = tile_upload_buffer,
                .offset = (Uint32)offset,
                .pixels_per_row = TILE_SIZE,
                .rows_per_layer = TILE_SIZE,
            };
            const SDL_GPUTextureRegion texture_region = {
                .texture = tile_textures.at(tile),
                .mip_level = 0,
                .layer = 0,
                .x = 0,
                .y = 0,
                .z = 0,
                .w = TILE_SIZE,
                .h = TILE_SIZE,
                .d = 1,
            };
            SDL_UploadToGPUTexture(upload_pass, &transfer_info, &texture_region, false);
            free_tile_upload_offset.push_back(offset);
        }

        allocated_tile_upload_offset.clear();
        SDL_EndGPUCopyPass(upload_pass);

        {
            ZoneScopedN("Submiting GPU command buffer");
            if (!SDL_SubmitGPUCommandBuffer(command_buffer)) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
                return false;
            }
        }

        tile_upload_buffer_ptr = (std::uint8_t *)SDL_MapGPUTransferBuffer(device, tile_upload_buffer, false);
    }

    // Download Tiles
    if (!allocated_tile_download_offset.empty()) {
        SDL_UnmapGPUTransferBuffer(device, tile_download_buffer);

        {  // Acquire GPU command buffer
            ZoneScopedN("Acquire GPU command buffer");
            command_buffer = SDL_AcquireGPUCommandBuffer(device);
            if (command_buffer == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
                return false;
            }
        }

        ZoneScopedN("Downloading Tiles");
        SDL_GPUCopyPass *download_pass = SDL_BeginGPUCopyPass(command_buffer);

        for (const auto &[tile, offset] : allocated_tile_download_offset) {
            ZoneScopedN("Downloading Tile");
            SDL_assert(tile_textures.contains(tile) && "Downloading missing texture");

            const SDL_GPUTextureTransferInfo destination = {
                .transfer_buffer = tile_download_buffer,
                .offset = (Uint32)offset,
                .pixels_per_row = TILE_SIZE,
                .rows_per_layer = TILE_SIZE,
            };
            const SDL_GPUTextureRegion source = {
                .texture = tile_textures.at(tile),
                .mip_level = 0,
                .layer = 0,
                .x = 0,
                .y = 0,
                .z = 0,
                .w = TILE_SIZE,
                .h = TILE_SIZE,
                .d = 1,
            };
            SDL_DownloadFromGPUTexture(download_pass, &source, &destination);
        }

        SDL_EndGPUCopyPass(download_pass);

        {
            ZoneScopedN("Submiting GPU command buffer");
            tile_download_fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer);
            if (tile_download_fence == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
                return false;
            }
        }

        {
            ZoneScopedN("Waiting for texture download");
            // This can be a silent trigger
            SDL_WaitForGPUFences(device, true, &tile_download_fence, 1);
            SDL_ReleaseGPUFence(device, tile_download_fence);
            tile_download_fence = nullptr;
            tile_download_buffer_ptr = (std::uint8_t *)SDL_MapGPUTransferBuffer(device, tile_download_buffer, false);
            for (const auto [tile, offset] : allocated_tile_download_offset) {
                tile_downloaded.insert(tile);
            }
        }
    }

    // Start painting the tiles
    if (!app->canvas.stroke_points.empty() && app->canvas.stroke_started) {
        paint_stroke_point_transfer_buffer_ptr =
            (std::uint8_t *)SDL_MapGPUTransferBuffer(device, paint_stroke_point_transfer_buffer, false);
        memset(paint_stroke_point_transfer_buffer_ptr, 0, MAX_PAINT_STROKE_POINTS * sizeof(Canvas::StrokePoint));
        memcpy(paint_stroke_point_transfer_buffer_ptr, app->canvas.stroke_points.data(),
               app->canvas.stroke_points.size() * sizeof(Canvas::StrokePoint));
        SDL_UnmapGPUTransferBuffer(device, paint_stroke_point_transfer_buffer);

        {  // Acquire GPU command buffer
            ZoneScopedN("Acquire GPU command buffer");
            command_buffer = SDL_AcquireGPUCommandBuffer(device);
            if (command_buffer == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
                return false;
            }
        }

        ZoneScopedN("Painting Tiles");

        // Copying data to the storage buffer
        SDL_GPUCopyPass *stroke_copy_pass = SDL_BeginGPUCopyPass(command_buffer);

        const SDL_GPUTransferBufferLocation source = {
            .transfer_buffer = paint_stroke_point_transfer_buffer,
            .offset = 0,
        };
        const SDL_GPUBufferRegion destination = {
            .buffer = paint_stroke_point_buffer,
            .offset = 0,
            .size = static_cast<Uint32>(app->canvas.stroke_points.size() * sizeof(Canvas::StrokePoint)),
            // .size = MAX_PAINT_STROKE_POINTS * sizeof(Canvas::StrokePoint),
        };
        SDL_UploadToGPUBuffer(stroke_copy_pass, &source, &destination, true);
        SDL_EndGPUCopyPass(stroke_copy_pass);

        const StrokeRenderData stroke_render_data = {
            .points_num = static_cast<std::uint32_t>(app->canvas.stroke_points.size()),
        };
        SDL_PushGPUComputeUniformData(command_buffer, 0, &stroke_render_data, sizeof(StrokeRenderData));
        const glm::ivec2 paint_compute_invocations = glm::ceil(glm::vec2(TILE_SIZE) / 32.0f);
        for (const auto &tile : app->canvas.stroke_tile_affected) {
            if (!tile_textures.contains(tile)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to get tile texture to paint");
                continue;
            }
            if (!app->canvas.tile_infos.contains(tile)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to get tile info to paint");
                continue;
            }
            const SDL_GPUStorageTextureReadWriteBinding paint_tile_binding[1] = {{
                .texture = tile_textures.at(tile),
                .mip_level = 0,
                .layer = 0,
            }};
            if (app->canvas.brush_mode) {
                SDL_GPUComputePass *paint_compute_pass =
                    SDL_BeginGPUComputePass(command_buffer, paint_tile_binding, 1, nullptr, 0);
                SDL_BindGPUComputePipeline(paint_compute_pass, paint_compute_pipeline);

                tile_render_data.position = app->canvas.tile_infos.at(tile).position;
                tile_render_data.size = glm::vec2(TILE_SIZE, TILE_SIZE);
                SDL_PushGPUComputeUniformData(command_buffer, 1, &tile_render_data, sizeof(TileRenderData));

                SDL_GPUTextureSamplerBinding samplerBinding = {
                    .texture = brush_texture,
                    .sampler = brush_sampler,
                };
                SDL_BindGPUComputeSamplers(paint_compute_pass, 0, &samplerBinding, 1);

                SDL_BindGPUComputeStorageBuffers(paint_compute_pass, 0, &paint_stroke_point_buffer, 1);

                SDL_DispatchGPUCompute(paint_compute_pass, paint_compute_invocations.x, paint_compute_invocations.y, 1);

                SDL_EndGPUComputePass(paint_compute_pass);

            } else if (app->canvas.eraser_mode) {
                SDL_GPUComputePass *erase_compute_pass =
                    SDL_BeginGPUComputePass(command_buffer, paint_tile_binding, 1, nullptr, 0);
                SDL_BindGPUComputePipeline(erase_compute_pass, erase_compute_pipeline);

                tile_render_data.position = app->canvas.tile_infos.at(tile).position;
                tile_render_data.size = glm::vec2(TILE_SIZE, TILE_SIZE);
                SDL_PushGPUComputeUniformData(command_buffer, 1, &tile_render_data, sizeof(TileRenderData));

                SDL_GPUTextureSamplerBinding samplerBinding = {
                    .texture = brush_texture,
                    .sampler = brush_sampler,
                };
                SDL_BindGPUComputeSamplers(erase_compute_pass, 0, &samplerBinding, 1);

                SDL_BindGPUComputeStorageBuffers(erase_compute_pass, 0, &paint_stroke_point_buffer, 1);

                SDL_DispatchGPUCompute(erase_compute_pass, paint_compute_invocations.x, paint_compute_invocations.y, 1);

                SDL_EndGPUComputePass(erase_compute_pass);
            }
        }
        app->canvas.stroke_tile_affected.clear();
        app->canvas.stroke_points.clear();

        {
            ZoneScopedN("Submiting GPU command buffer");
            auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command_buffer);
            if (fence == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
                return false;
            }
            SDL_WaitForGPUFences(device, true, &fence, 1);
            SDL_ReleaseGPUFence(device, fence);
        }
    }

    if (app->window_size.x > 0 && app->window_size.y > 0 && !app->hidden) {
        {  // Compute view matrix
            ZoneScopedN("Compute view matrix");

            auto view = glm::mat4(1.0f);
            view = glm::translate(view, glm::vec3(app->canvas.view.pan, 0.0f));
            view = glm::rotate(view, app->canvas.view.rotation, glm::vec3(0.0f, 0.0f, 1.0f));
            view = glm::scale(view, glm::vec3(app->canvas.view.zoom_amount, app->canvas.view.zoom_amount, 1.0f));
            viewport_render_data.view = view;
        }

        {  // Acquire GPU command buffer
            ZoneScopedN("Acquire GPU command buffer");
            command_buffer = SDL_AcquireGPUCommandBuffer(device);
            if (command_buffer == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
                return false;
            }
        }

        {  // Wait and acquire GPU swapchain texture
            ZoneScopedN("Wait and acquire GPU swapchain texture");
            SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer, app->window, &swapchain_texture, nullptr, nullptr);
            if (swapchain_texture == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire swapchain texture: %s", SDL_GetError());
                return false;
            }
        }

        // Get all layers that needs redrawing
        std::vector<LayerInfo> layer_rendering;
        {
            ZoneScopedN("Layer Culling");
            {
                ZoneScopedN("Filtering layers");
                layer_rendering.reserve(app->canvas.layer_infos.size());
                for (const auto &[layer, info] : app->canvas.layer_infos) {
                    if (!info.visible || info.opacity == 0.0f) {
                        continue;
                    }
                    if (app->canvas.layer_to_delete.contains(layer)) {
                        continue;
                    }

                    SDL_assert(layer_textures.contains(layer) && "Layer texture not found");
                    // would be could to add bubbles sort right here
                    layer_rendering.push_back(info);
                }
            }
            {
                ZoneScopedN("Sorting layer by depth");
                // Sort the layers based on depth
                std::ranges::sort(layer_rendering, [](LayerInfo &a, LayerInfo &b) { return a.depth > b.depth; });
            }
        }

        {  // Tile Rendering
            ZoneScopedN("Rendering Tile");
            for (const auto &layer_info : layer_rendering) {
                ZoneScopedN("Render Tile");
                // TODO: only redraw changed & visible tiles
                const SDL_GPUColorTargetInfo target_info = {
                    .texture = layer_textures.at(layer_info.layer),
                    .clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f},
                    .load_op = SDL_GPU_LOADOP_CLEAR,
                    .store_op = SDL_GPU_STOREOP_STORE,
                };
                SDL_GPURenderPass *render_pass = SDL_BeginGPURenderPass(command_buffer, &target_info, 1, nullptr);
                SDL_BindGPUGraphicsPipeline(render_pass, tile_graphics_pipeline);

                SDL_PushGPUVertexUniformData(command_buffer, 0, &viewport_render_data, sizeof(ViewportRenderData));

                for (const auto &tile : app->canvas.layer_tiles.at(layer_info.layer)) {
                    if (app->canvas.tile_to_delete.contains(tile) || app->canvas.tile_to_unload.contains(tile)) {
                        continue;
                    }

                    const TileInfo tile_info = app->canvas.tile_infos.at(tile);
                    tile_render_data.position = tile_info.position;
                    tile_render_data.size = glm::vec2(TILE_SIZE, TILE_SIZE);
                    SDL_PushGPUVertexUniformData(command_buffer, 1, &tile_render_data, sizeof(TileRenderData));

                    const SDL_GPUTextureSamplerBinding samplers[] = {{
                        .texture = tile_textures.at(tile),
                        .sampler = tile_sampler,
                    }};
                    SDL_BindGPUFragmentSamplers(render_pass, 0, samplers, 1);

                    SDL_DrawGPUPrimitives(render_pass, 4, 1, 0, 0);
                    last_rendered_tiles_num++;
                }

                SDL_EndGPURenderPass(render_pass);
            }
        }

        {  // Layer rendering
            ZoneScopedN("Layer blending and rendering");

            auto rgb = glm::vec3(app->bg_color);
            rgb = glm::mix(glm::pow((rgb + glm::vec3(0.055)) * glm::vec3(1.0 / 1.055), glm::vec3(2.4)),
                           rgb * glm::vec3(1.0 / 12.92), glm::lessThanEqual(rgb, glm::vec3(0.04045)));
            rgb *= app->bg_color.a;

            const SDL_GPUColorTargetInfo target_info = {
                .texture = canvas_texture,
                .clear_color = SDL_FColor{rgb.r, rgb.g, rgb.b, app->bg_color.a},
                .load_op = SDL_GPU_LOADOP_CLEAR,
                .store_op = SDL_GPU_STOREOP_STORE,
            };
            SDL_GPURenderPass *render_pass = SDL_BeginGPURenderPass(command_buffer, &target_info, 1, nullptr);
            SDL_EndGPURenderPass(render_pass);

            const glm::ivec2 merge_compute_invocations = glm::ceil(glm::vec2(app->window_size) / 32.0f);
            MergeRenderData merge_render_data = {
                .src_blend_mode = static_cast<std::uint32_t>(BlendMode::Normal),
                .src_opacity = 1.0f,
                .src_pos = glm::vec2(0.0),
                .src_size = app->window_size,
                .dst_blend_mode = static_cast<std::uint32_t>(BlendMode::Normal),
                .dst_opacity = 1.0f,
                .dst_pos = glm::vec2(0.0),
                .dst_size = app->window_size,
            };
            const SDL_GPUStorageTextureReadWriteBinding merge_layer_binding[1] = {{
                .texture = canvas_texture,
                .mip_level = 0,
                .layer = 0,
            }};
            SDL_GPUComputePass *merge_compute_pass =
                SDL_BeginGPUComputePass(command_buffer, merge_layer_binding, 1, nullptr, 0);
            if (merge_compute_pass == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to create merge compute pass: %s", SDL_GetError());
                return false;
            }
            SDL_BindGPUComputePipeline(merge_compute_pass, merge_compute_pipeline);

            for (const auto &layer_info : layer_rendering) {
                ZoneScopedN("Blend layer to canvas");
                merge_render_data.src_blend_mode = static_cast<std::uint32_t>(layer_info.blend_mode);
                merge_render_data.src_opacity = layer_info.opacity;
                // Maybe dividing the buffer into a dst and src could be benificial
                SDL_PushGPUComputeUniformData(command_buffer, 0, &merge_render_data, sizeof(MergeRenderData));

                const SDL_GPUTextureSamplerBinding samplers[] = {{
                    .texture = layer_textures.at(layer_info.layer),
                    .sampler = layer_sampler,
                }};
                SDL_BindGPUComputeSamplers(merge_compute_pass, 0, samplers, 1);
                SDL_DispatchGPUCompute(merge_compute_pass, merge_compute_invocations.x, merge_compute_invocations.y, 1);
            }

            SDL_EndGPUComputePass(merge_compute_pass);
        }

        {
            ZoneScopedN("Render canvas texture");
            const SDL_GPUColorTargetInfo target_info = {
                .texture = swapchain_texture,
                .clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f},
                .load_op = SDL_GPU_LOADOP_CLEAR,
                .store_op = SDL_GPU_STOREOP_STORE,
            };
            SDL_GPURenderPass *render_pass = SDL_BeginGPURenderPass(command_buffer, &target_info, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(render_pass, layer_graphics_pipeline);

            const SDL_GPUTextureSamplerBinding samplers[] = {{
                .texture = canvas_texture,
                .sampler = layer_sampler,
            }};
            SDL_BindGPUFragmentSamplers(render_pass, 0, samplers, 1);

            SDL_DrawGPUPrimitives(render_pass, 3, 1, 0, 0);

            SDL_EndGPURenderPass(render_pass);
        }

        {  // UI Rendering
            ZoneScopedN("UI rendering");
            ImDrawData *draw_data = ImGui::GetDrawData();

            ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);
            const SDL_GPUColorTargetInfo target_info = {
                .texture = swapchain_texture,
                .load_op = SDL_GPU_LOADOP_LOAD,
                .store_op = SDL_GPU_STOREOP_STORE,
            };
            SDL_GPURenderPass *render_pass = SDL_BeginGPURenderPass(command_buffer, &target_info, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, render_pass);
            SDL_EndGPURenderPass(render_pass);
        }

        {
            ZoneScopedN("Submiting GPU command buffer");
            if (!SDL_SubmitGPUCommandBuffer(command_buffer)) {
                SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
                return false;
            }
        }
    }

    return true;
}

bool Renderer::Resize() {
    ZoneScoped;
    viewport_render_data.projection =
        glm::ortho(-((float)app->window_size.x / 2.0f), ((float)app->window_size.x / 2.0f),
                   -((float)app->window_size.y / 2.0f), ((float)app->window_size.y / 2.0f), 0.0f, 1.0f);

    std::vector<Layer> layers;
    layers.reserve(layer_textures.size());
    for (const auto &[layer, texture] : layer_textures) {
        layers.push_back(layer);
    }

    SDL_ReleaseGPUTexture(device, canvas_texture);
    const SDL_GPUTextureCreateInfo canvas_texture_create_info = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = texture_format,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET |
                 SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE,
        .width = (Uint32)app->window_size.x,
        .height = (Uint32)app->window_size.y,
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    canvas_texture = SDL_CreateGPUTexture(device, &canvas_texture_create_info);
    if (canvas_texture == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create canvas layer texture");
        return false;
    }

    for (auto layer : layers) {
        DeleteLayerTexture(layer);
        CreateLayerTexture(layer);
    }

    return true;
}

bool Renderer::CanQuit() { return allocated_tile_download_offset.empty(); }

void Renderer::Quit() {
    ZoneScoped;

    SDL_UnmapGPUTransferBuffer(device, paint_stroke_point_transfer_buffer);
    SDL_ReleaseGPUTexture(device, brush_texture);
    SDL_ReleaseGPUSampler(device, brush_sampler);
    SDL_ReleaseGPUTransferBuffer(device, paint_stroke_point_transfer_buffer);
    SDL_ReleaseGPUBuffer(device, paint_stroke_point_buffer);
    SDL_ReleaseGPUComputePipeline(device, erase_compute_pipeline);
    SDL_ReleaseGPUComputePipeline(device, paint_compute_pipeline);

    SDL_ReleaseGPUComputePipeline(device, merge_compute_pipeline);

    for (const auto &[tile, texture] : tile_textures) {
        SDL_ReleaseGPUTexture(device, texture);
    }
    SDL_UnmapGPUTransferBuffer(device, tile_download_buffer);
    SDL_ReleaseGPUTransferBuffer(device, tile_download_buffer);
    SDL_UnmapGPUTransferBuffer(device, tile_upload_buffer);
    SDL_ReleaseGPUTransferBuffer(device, tile_upload_buffer);
    SDL_ReleaseGPUTransferBuffer(device, tile_blank_texture_buffer);
    SDL_ReleaseGPUSampler(device, tile_sampler);
    SDL_ReleaseGPUGraphicsPipeline(device, tile_graphics_pipeline);
    SDL_ReleaseGPUShader(device, tile_vertex_shader);
    SDL_ReleaseGPUShader(device, tile_fragment_shader);

    for (const auto &[layer, texture] : layer_textures) {
        SDL_ReleaseGPUTexture(device, texture);
    }
    SDL_ReleaseGPUTexture(device, canvas_texture);
    SDL_ReleaseGPUSampler(device, layer_sampler);
    SDL_ReleaseGPUGraphicsPipeline(device, layer_graphics_pipeline);
    SDL_ReleaseGPUShader(device, layer_vertex_shader);
    SDL_ReleaseGPUShader(device, layer_fragment_shader);

    SDL_ReleaseWindowFromGPUDevice(device, app->window);
    SDL_DestroyGPUDevice(device);

    device = nullptr;
}

bool Renderer::CreateLayerTexture(const Layer layer) {
    SDL_assert(!layer_textures.contains(layer));
    SDL_assert(app->window_size.x > 0);
    SDL_assert(app->window_size.y > 0);

    const SDL_GPUTextureCreateInfo texture_create_info = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = texture_format,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
        .width = (Uint32)app->window_size.x,
        .height = (Uint32)app->window_size.y,
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &texture_create_info);
    if (texture == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create layer texture");
        return false;
    }

    layer_textures[layer] = texture;
    return true;
}

void Renderer::DeleteLayerTexture(const Layer layer) {
    ZoneScoped;
    SDL_assert(layer_textures.contains(layer));
    SDL_ReleaseGPUTexture(device, layer_textures.at(layer));
    layer_textures.erase(layer);
}

std::optional<Renderer::TileTextureError> Renderer::CreateTileTexture(const Tile tile) {
    ZoneScoped;
    SDL_assert(!tile_textures.contains(tile));

    const SDL_GPUTextureCreateInfo texture_create_info = {
        .type = SDL_GPU_TEXTURETYPE_2D,
        .format = texture_format,
        .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE,
        .width = (Uint32)TILE_SIZE,
        .height = (Uint32)TILE_SIZE,
        .layer_count_or_depth = 1,
        .num_levels = 1,
        .sample_count = SDL_GPU_SAMPLECOUNT_1,
    };
    SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &texture_create_info);
    if (texture == nullptr) {
        return TileTextureError::Unknwon;
    } else {
        tile_textures[tile] = texture;
        tile_texture_uninitialized.insert(tile);
    }

    return std::nullopt;
}

std::optional<Renderer::TileTextureError> Renderer::UploadTileTexture(const Tile tile,
                                                                      std::span<const uint8_t> pixels) {
    ZoneScoped;
    if (!tile_textures.contains(tile)) {
        return TileTextureError::MissingTexture;
    }

    if (free_tile_upload_offset.empty()) {
        return TileTextureError::UploadSlotMissing;
    }

    if (!allocated_tile_upload_offset.contains(tile)) {
        size_t offset = free_tile_upload_offset.back();
        SDL_assert(offset <= (TILE_MAX_UPLOAD_TRANSFER - 1) * TILE_SIZE * TILE_SIZE * 4);
        free_tile_upload_offset.pop_back();
        allocated_tile_upload_offset[tile] = offset;
    }

    auto *dst = (uint8_t *)(tile_upload_buffer_ptr + allocated_tile_upload_offset.at(tile));
    memcpy(dst, pixels.data(), pixels.size_bytes());

    return std::nullopt;
}

void Renderer::ReleaseTileTexture(const Tile tile) {
    ZoneScoped;
    SDL_assert(tile_textures.contains(tile) && "Tile does not exists");
    SDL_ReleaseGPUTexture(device, tile_textures.at(tile));
    tile_textures.erase(tile);
}

// TODO: transform into a single command buffer action
bool Renderer::MergeTileTextures(const Tile over_tile, const Tile below_tile) {
    ZoneScoped;
    SDL_assert(tile_textures.contains(over_tile));
    SDL_assert(tile_textures.contains(below_tile));

    const auto over_tile_info = app->canvas.tile_infos.at(over_tile);
    const auto below_tile_info = app->canvas.tile_infos.at(below_tile);

    const auto over_layer_info = app->canvas.layer_infos.at(over_tile_info.layer);
    const auto below_layer_info = app->canvas.layer_infos.at(below_tile_info.layer);

    SDL_GPUCommandBuffer *command_buffer = nullptr;
    {  // Acquire GPU command buffer
        ZoneScopedN("Acquire GPU command buffer");
        command_buffer = SDL_AcquireGPUCommandBuffer(device);
        if (command_buffer == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to acquire gpu command buffer: %s", SDL_GetError());
            return false;
        }
    }

    {  // Layer rendering
        constexpr auto tile_size = glm::vec2(TILE_SIZE);
        ZoneScopedN("Layer blending and rendering");
        const glm::ivec2 merge_compute_invocations = glm::ceil(tile_size / 32.0f);
        MergeRenderData merge_render_data = {
            .src_blend_mode = static_cast<std::uint32_t>(over_layer_info.blend_mode),
            .src_opacity = over_layer_info.opacity,
            .src_pos = glm::vec2(0.0),
            .src_size = tile_size,
            .dst_blend_mode = static_cast<std::uint32_t>(below_layer_info.blend_mode),
            .dst_opacity = below_layer_info.opacity,
            .dst_pos = glm::vec2(0.0),
            .dst_size = tile_size,
        };
        const SDL_GPUStorageTextureReadWriteBinding merge_layer_binding[1] = {{
            .texture = tile_textures.at(below_tile),
            .mip_level = 0,
            .layer = 0,
        }};
        SDL_GPUComputePass *merge_compute_pass =
            SDL_BeginGPUComputePass(command_buffer, merge_layer_binding, 1, nullptr, 0);
        if (merge_compute_pass == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to create merge compute pass: %s", SDL_GetError());
            return false;
        }
        SDL_BindGPUComputePipeline(merge_compute_pass, merge_compute_pipeline);
        SDL_PushGPUComputeUniformData(command_buffer, 0, &merge_render_data, sizeof(MergeRenderData));

        const SDL_GPUTextureSamplerBinding samplers[] = {{
            .texture = tile_textures.at(over_tile),
            .sampler = tile_sampler,
        }};
        SDL_BindGPUComputeSamplers(merge_compute_pass, 0, samplers, 1);
        SDL_DispatchGPUCompute(merge_compute_pass, merge_compute_invocations.x, merge_compute_invocations.y, 1);

        SDL_EndGPUComputePass(merge_compute_pass);
    }

    {
        ZoneScopedN("Submiting GPU command buffer");
        if (!SDL_SubmitGPUCommandBuffer(command_buffer)) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Failed to submit gpu command buffer: %s", SDL_GetError());
            return false;
        }
    }

    return true;
}

bool Renderer::DownloadTileTexture(Tile tile) {
    ZoneScoped;
    SDL_assert(tile > 0 && "Tile is invalid");
    SDL_assert(tile_textures.contains(tile));
    SDL_assert(!allocated_tile_download_offset.contains(tile) && "??");
    SDL_assert(!tile_downloaded.contains(tile));

    if (free_tile_download_offset.empty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "No slot buffer slot for donwloading tile");
        return false;
    }

    const auto tile_offset = free_tile_download_offset.back();
    free_tile_download_offset.pop_back();
    SDL_assert(tile_offset <= (TILE_MAX_DOWNLOAD_TRANSFER - 1) * TILE_SIZE * TILE_SIZE * 4);

    allocated_tile_download_offset[tile] = tile_offset;

    return true;
}

bool Renderer::IsTileTextureDownloaded(Tile tile) const {
    ZoneScoped;
    SDL_assert(tile > 0 && "Tile is invalid");
    SDL_assert(allocated_tile_download_offset.contains(tile) && "Tile not allocated");

    return tile_downloaded.contains(tile);
}

bool Renderer::CopyTileTextureDownloaded(const Tile tile, std::vector<uint8_t> &tile_texture) {
    ZoneScoped;
    SDL_assert(tile > 0 && "Tile is invalid");
    SDL_assert(IsTileTextureDownloaded(tile));

    tile_texture.resize(TILE_SIZE * TILE_SIZE * 4);

    SDL_assert(tile_download_buffer_ptr != nullptr);
    SDL_assert(allocated_tile_download_offset.at(tile) >= 0);
    SDL_assert(allocated_tile_download_offset.at(tile) <=
               (TILE_MAX_DOWNLOAD_TRANSFER - 1) * (TILE_SIZE * TILE_SIZE * 4));
    memcpy(tile_texture.data(), tile_download_buffer_ptr + allocated_tile_download_offset.at(tile),
           (TILE_SIZE * TILE_SIZE * 4));

    free_tile_download_offset.push_back(allocated_tile_download_offset.at(tile));
    tile_downloaded.erase(tile);
    allocated_tile_download_offset.erase(tile);

    return true;
}

}  // namespace Midori
