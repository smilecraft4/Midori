#ifndef MIDORI_RENDERER_H
#define MIDORI_RENDERER_H

#include <SDL3/SDL_gpu.h>
#include <cstdint>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <unordered_map>
#include <vector>

#include "SDL3/SDL_stdinc.h"
#include "glm/ext/matrix_float4x4.hpp"
#include "glm/ext/vector_float2.hpp"
#include "midori/types.h"

namespace Midori {

class App;

class Renderer {
public:
  Renderer(const Renderer &) = delete;
  Renderer(Renderer &&) = delete;
  Renderer &operator=(const Renderer &) = delete;
  Renderer &operator=(Renderer &&) = delete;

  Renderer(App *app);
  ~Renderer() = default;

  bool Init();
  bool Render();
  bool Resize();
  void Quit();

  bool InitLayers();
  bool InitTiles();

  bool CreateLayerTexture(Layer layer);
  void DeleteLayerTexture(Layer layer);

  bool CreateTileTexture(Tile tile);
  bool UploadTileTexture(Tile tile, const std::vector<Uint8> &raw_pixels);
  bool DownloadTileTexture(Tile tile);
  void DeleteTileTexture(Tile tile);

  App *app;

  // Common data
  SDL_GPUDevice *device = nullptr;
  SDL_GPUTextureFormat texture_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  SDL_GPUTextureFormat swapchain_format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
  SDL_GPUTexture *canvas_texture = nullptr;

  struct ViewportRenderData {
    glm::mat4 projection = glm::mat4(1.0f);
    glm::mat4 view = glm::mat4(1.0f);
  } viewport_render_data;

  // Layer data
  struct LayerRenderData {
    float opacity = 1.0f;
    std::uint32_t blend_mode = (uint32_t)BlendMode::Normal;
  } layer_render_data;

  SDL_GPUShader *layer_vertex_shader = nullptr;
  SDL_GPUShader *layer_fragment_shader = nullptr;
  SDL_GPUGraphicsPipeline *layer_graphics_pipeline = nullptr;
  SDL_GPUSampler *layer_sampler = nullptr;
  std::unordered_map<Layer, SDL_GPUTexture *> layer_textures;
  size_t last_layer_rendered_num = 0;

  // Tile data
  struct TileRenderData {
    glm::vec2 position = glm::vec2(0.0f, 0.0f);
    glm::vec2 size = glm::vec2(TILE_SIZE, TILE_SIZE); // Maybe hard code this
  } tile_render_data;

  SDL_GPUShader *tile_vertex_shader = nullptr;
  SDL_GPUShader *tile_fragment_shader = nullptr;
  SDL_GPUGraphicsPipeline *tile_graphics_pipeline = nullptr;
  SDL_GPUSampler *tile_sampler = nullptr;
  std::unordered_map<Tile, SDL_GPUTexture *> tile_textures;
  size_t last_rendered_tiles_num = 0;

  // Tile download/upload
  static constexpr size_t TILE_MAX_UPLOAD_TRANSFER = 32;
  SDL_GPUTransferBuffer *tile_upload_buffer = nullptr;
  std::uint8_t *tile_upload_buffer_ptr = nullptr;
  std::unordered_map<Tile, Uint32> allocated_tile_upload_offset;
  std::vector<Uint32> free_tile_upload_offset;

  static constexpr size_t TILE_MAX_DOWNLOAD_TRANSFER = 32;
  SDL_GPUTransferBuffer *tile_download_buffer = nullptr;
  std::uint8_t *tile_download_buffer_ptr = nullptr;
  std::unordered_map<Tile, Uint32> allocated_tile_download_offset;
  std::vector<Uint32> free_tile_download_offset;

  // OPERATIONS

  // Texture merging
  bool InitMerge();
  struct MergeRenderData {
    std::uint32_t src_blend_mode;
    float src_opacity;
    glm::ivec2 src_pos;
    glm::ivec2 src_size;

    std::uint32_t dst_blend_mode;
    float dst_opacity;
    glm::ivec2 dst_pos;
    glm::ivec2 dst_size;
  };
  SDL_GPUComputePipeline *merge_compute_pipeline = nullptr;

  bool InitPaint();
  static constexpr size_t MAX_PAINT_STROKE_POINTS = 2048;
  SDL_GPUBuffer *paint_stroke_point_buffer = nullptr;
  SDL_GPUComputePipeline *paint_compute_pipeline = nullptr;
  SDL_GPUTransferBuffer *paint_stroke_point_transfer_buffer = nullptr;
  std::uint8_t *paint_stroke_point_transfer_buffer_ptr = nullptr;
};
} // namespace Midori

#endif
