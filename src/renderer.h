#ifndef MIDORI_RENDERER_H
#define MIDORI_RENDERER_H

#include <span>

#include "app.h"

#ifdef RENDERER_SDL3
typedef struct SDL_Window SDL_Window;
#endif

#ifdef RENDERER_WIN32_GL
using HINSTANCE = struct HINSTANCE__ *;
using HWND = struct HWND__ *;
#endif

namespace Renderer {
enum class Result : uint8_t {
  Success,
  MissingLayerTexture,
  MissingTileTexture,
  Unknown,
};

#ifdef RENDERER_SDL3
Result InitSDL3(SDL_Window *window);
void TerminateSDL3();
#endif

Result Resize(int width, int height);

std::expected<App::Tile, Result>
CreateTileTexture(std::optional<std::span<uint8_t>> texture);
Result UploadTileTexture(App::Tile tile, std::span<uint8_t> texture);
Result DownloadTileTexture(App::Tile tile);
std::expected<bool, Result> IsTileTextureDownloaded(App::Tile tile);
Result MoveTileTextureDownload(App::Tile tile,
                               std::vector<uint8_t> destination);
Result DeleteTileTexture(App::Tile tile);
Result MergeTile(App::Tile over, App::Tile under, App::Tile destination);

std::expected<App::Layer, Result> CreateLayerTexture();
Result ClearTileLayerRenderTarget();
Result DeleteLayerTexture(App::Layer layer);

Result RenderTile(App::Tile tile);
Result RenderLayer(App::Layer layer);

Result NewFrame();
Result Render();
} // namespace Renderer

/*

#include <cstdint>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "SDL3/SDL_stdinc.h"
#include "glm/ext/matrix_float4x4.hpp"
#include "glm/ext/vector_float2.hpp"
#include "midori/types.h"

namespace Midori {

class App;

class Renderer {
  public:
    Renderer(const Renderer&)            = delete;
    Renderer(Renderer&&)                 = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer& operator=(Renderer&&)      = delete;

    Renderer(App* app);
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
    bool UploadTileTexture(Tile tile, const std::vector<Uint8>& raw_pixels);
    void DeleteTileTexture(Tile tile);
    bool MergeTileTextures(Tile over_tile, Tile below_tile);

    App* app;

    std::unordered_set<Tile>  tile_to_draw;
    std::unordered_set<Layer> layer_to_draw;

    // Common data
    SDL_GPUDevice*               device           = nullptr;
    SDL_GPUTextureFormat         texture_format   =
SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; SDL_GPUTextureFormat swapchain_format =
SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM; SDL_GPUTexture* canvas_texture   =
nullptr; std::vector<SDL_GPUTexture*> textures_to_delete;

    struct ViewportRenderData {
        glm::mat4 projection = glm::mat4(1.0f);
        glm::mat4 view       = glm::mat4(1.0f);
    } viewport_render_data;
    bool view_changed = false;

    // Layer data
    struct LayerRenderData {
        float         opacity    = 1.0f;
        std::uint32_t blend_mode = (uint32_t)BlendMode::Normal;
    } layer_render_data;

    SDL_GPUShader*                             layer_vertex_shader     =
nullptr; SDL_GPUShader*                             layer_fragment_shader   =
nullptr; SDL_GPUGraphicsPipeline*                   layer_graphics_pipeline =
nullptr; SDL_GPUSampler*                            layer_sampler           =
nullptr; std::unordered_map<Layer, SDL_GPUTexture*> layer_textures; size_t
last_layer_rendered_num = 0;

    // Tile data
    struct TileRenderData {
        glm::vec2 position = glm::vec2(0.0f, 0.0f);
        glm::vec2 size     = glm::vec2(TILE_SIZE, TILE_SIZE); // Maybe hard code
this } tile_render_data;

    SDL_GPUShader*                            tile_vertex_shader     = nullptr;
    SDL_GPUShader*                            tile_fragment_shader   = nullptr;
    SDL_GPUGraphicsPipeline*                  tile_graphics_pipeline = nullptr;
    SDL_GPUSampler*                           tile_sampler           = nullptr;
    std::unordered_map<Tile, SDL_GPUTexture*> tile_textures;
    size_t                                    last_rendered_tiles_num = 0;

    // Tile upload
    static constexpr size_t          TILE_MAX_UPLOAD_TRANSFER = 32;
    SDL_GPUTransferBuffer*           tile_upload_buffer       = nullptr;
    std::uint8_t*                    tile_upload_buffer_ptr   = nullptr;
    std::unordered_map<Tile, Uint32> allocated_tile_upload_offset;
    std::vector<Uint32>              free_tile_upload_offset;

    // Tile download

    bool DownloadTileTexture(Tile tile);
    bool IsTileTextureDownloaded(Tile tile) const;
    bool CopyTileTextureDownloaded(Tile tile, std::vector<uint8_t>&
tile_texture);

    static constexpr size_t          TILE_MAX_DOWNLOAD_TRANSFER = 32;
    SDL_GPUTransferBuffer*           tile_download_buffer       = nullptr;
    std::uint8_t*                    tile_download_buffer_ptr   = nullptr;
    std::unordered_map<Tile, Uint32> allocated_tile_download_offset;
    std::unordered_set<Tile>         tile_downloaded;
    std::vector<Uint32>              free_tile_download_offset;
    SDL_GPUFence*                    tile_download_fence = nullptr; // TODO: Use
multiple fences

    // OPERATIONS

    // Texture merging
    bool InitMerge();
    struct MergeRenderData {
        std::uint32_t src_blend_mode;
        float         src_opacity;
        glm::ivec2    src_pos;
        glm::ivec2    src_size;

        std::uint32_t dst_blend_mode;
        float         dst_opacity;
        glm::ivec2    dst_pos;
        glm::ivec2    dst_size;
    };
    SDL_GPUComputePipeline* merge_compute_pipeline = nullptr;

    bool InitPaint();
    struct StrokeRenderData {
        std::uint32_t points_num = 0;
    };
    static constexpr size_t MAX_PAINT_STROKE_POINTS                = 2048;
    SDL_GPUBuffer*          paint_stroke_point_buffer              = nullptr;
    SDL_GPUComputePipeline* paint_compute_pipeline                 = nullptr;
    SDL_GPUTransferBuffer*  paint_stroke_point_transfer_buffer     = nullptr;
    std::uint8_t*           paint_stroke_point_transfer_buffer_ptr = nullptr;
};
} // namespace Midori
*/
#endif
