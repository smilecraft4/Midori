#pragma once

#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>

#include "types.h"

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
    bool CanQuit();
    void Quit();

    bool InitLayers();
    bool InitTiles();

    bool CreateLayerTexture(Layer layer);
    void DeleteLayerTexture(Layer layer);

    enum class TileTextureError : uint8_t {
        None,
        MissingTexture,
        UploadSlotMissing,

        Unknwon,
    };

    /**
     * @brief Create a Tile texture on the gpu, The will be initialized by the time the gpu start to render a frame
     * before this the tile texture is undefined and must not be used for anything
     *
     * @param tile
     * @return std::optional<TileError>
     */
    std::optional<TileTextureError> CreateTileTexture(Tile tile);

    /**
     * @brief Set the texture pixels of the specified tile. It must be of size `TILE_SIZE * TILE_SIZE * 4`, This
     * operation may be completed only after 1~2 frame
     *
     * @param tile
     * @param pixels
     * @return std::optional<TileError>
     */
    std::optional<TileTextureError> UploadTileTexture(Tile tile, std::span<const uint8_t> pixels);

    void ReleaseTileTexture(Tile tile);
    bool MergeTileTextures(Tile over_tile, Tile below_tile);

    App *app;

    std::unordered_set<Tile> tile_to_draw;
    std::unordered_set<Layer> layer_to_draw;

    // Common data
    SDL_GPUDevice *device = nullptr;
    SDL_GPUTextureFormat texture_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUTextureFormat swapchain_format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    SDL_GPUTexture *canvas_texture = nullptr;
    std::vector<SDL_GPUTexture *> textures_to_delete;

    struct ViewportRenderData {
        glm::mat4 projection = glm::mat4(1.0f);
        glm::mat4 view = glm::mat4(1.0f);
    } viewport_render_data;
    bool view_changed = false;

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
        glm::vec2 size = glm::vec2(TILE_SIZE, TILE_SIZE);  // Maybe hard code this
    } tile_render_data;

    SDL_GPUShader *tile_vertex_shader = nullptr;
    SDL_GPUShader *tile_fragment_shader = nullptr;
    SDL_GPUGraphicsPipeline *tile_graphics_pipeline = nullptr;
    SDL_GPUSampler *tile_sampler = nullptr;
    std::unordered_map<Tile, SDL_GPUTexture *> tile_textures;
    std::unordered_set<Tile> tile_texture_uninitialized;
    size_t last_rendered_tiles_num = 0;

    // Tile upload
    static constexpr size_t TILE_MAX_UPLOAD_TRANSFER = 32;
    SDL_GPUTransferBuffer *tile_upload_buffer = nullptr;
    SDL_GPUTransferBuffer *tile_blank_texture_buffer = nullptr;
    uint8_t *tile_upload_buffer_ptr = nullptr;
    std::unordered_map<Tile, size_t> allocated_tile_upload_offset;
    std::vector<size_t> free_tile_upload_offset;

    // Tile download

    bool DownloadTileTexture(Tile tile);
    bool IsTileTextureDownloaded(Tile tile) const;
    bool CopyTileTextureDownloaded(Tile tile, std::vector<uint8_t> &tile_texture);
    SDL_GPUTexture *DuplicateTileTexture(SDL_GPUCopyPass *copyPass, SDL_GPUTexture *tileTexture) const;

    static constexpr size_t TILE_MAX_DOWNLOAD_TRANSFER = 32;
    SDL_GPUTransferBuffer *tile_download_buffer = nullptr;
    uint8_t *tile_download_buffer_ptr = nullptr;
    std::unordered_map<Tile, size_t> allocated_tile_download_offset;
    std::unordered_set<Tile> tile_downloaded;
    std::vector<size_t> free_tile_download_offset;
    SDL_GPUFence *tile_download_fence = nullptr;  // TODO: Use multiple fences

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
    struct StrokeRenderData {
        std::uint32_t points_num = 0;
    };
    static constexpr size_t MAX_PAINT_STROKE_POINTS = 2048;
    SDL_GPUBuffer *paint_stroke_point_buffer = nullptr;
    SDL_GPUComputePipeline *paint_compute_pipeline = nullptr;
    SDL_GPUComputePipeline *erase_compute_pipeline = nullptr;
    SDL_GPUTransferBuffer *paint_stroke_point_transfer_buffer = nullptr;
    SDL_GPUTexture *brush_texture = nullptr;
    SDL_GPUSampler *brush_sampler = nullptr;
    std::uint8_t *paint_stroke_point_transfer_buffer_ptr = nullptr;

    SDL_GPUShaderFormat shaderFormat;
    std::string layerVertFilename;
    std::string layerFragFilename;
    std::string tileVertFilename;
    std::string tileFragFilename;
    std::string eraseCompFilename;
    std::string paintCompFilename;
    std::string mergeCompFilename;
};
}  // namespace Midori
