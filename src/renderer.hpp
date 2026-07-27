#pragma once

#include "defines.hpp"
#include "errors.hpp"
#include "tiles.hpp"
#include "colors.hpp"

#include <SDL3/SDL_gpu.h>
#include <cstddef>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <cstdint>
#include <optional>
#include <vector>

namespace Midori {

struct App;
struct Window;
struct CanvasView;
struct CanvasFile;

struct StrokePoint {
    glm::vec4 color = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    glm::vec2 position;
    float radius = 8.0f;
    float flow = 0.5f;
    float hardness = 0.5f;
    float pad[3];

    static StrokePoint Lerp(StrokePoint start, StrokePoint end, float t);
};

struct Renderer {
    using TileTextureDownload = size_t;
    using TileTextureUpload = size_t;

    enum class TextureDownloadState : uint8_t {
        None,        // slot available for use
        Pending,     // waiting to start downloading data
        Downloading, // retrieving the data, looking at fence to know if it can copied
    };

    enum class TextureUploadState : uint8_t {
        None,      // slot available for use
        Reserved,  // slot assigned to something
        Pending,   // waiting to be uploaded
        Uploading, // uploading, when done can be cleared (looking if the fence is nullptr)
    };

    struct ViewportRenderData {
        glm::mat4 projection = glm::mat4(1.0f);
        glm::mat4 view = glm::mat4(1.0f);
    };

    struct LayerRenderData {
        float opacity = 1.0f;
        std::uint32_t blendMode = (uint32_t)BlendMode::Alpha;
    };

    struct TileRenderData {
        glm::vec2 position = glm::vec2(0.0f, 0.0f);
        glm::vec2 size = glm::vec2(TILE_WIDTH, TILE_HEIGHT);
    };

    struct MergeRenderData {
        std::uint32_t srcBlendMode;
        float srcOpacity;
    };

    struct StrokeRenderData {
        std::uint32_t pointsNum = 0;
    };

    struct TileTextureDownloadInfo {
        std::atomic<TextureDownloadState> state{TextureDownloadState::None};
        Tile tile;
        std::byte* data{};
        size_t fence;
    };

    struct TileTextureUploadInfo {
        std::atomic<TextureUploadState> state{TextureUploadState::None};
        Tile tile;
        std::byte* data{};
        size_t fence;
    };

    struct MergeInfo {
        BlendMode blendMode;
        float opacity;
    };

    Renderer(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    Renderer(App* app);
    ~Renderer() = default;

    bool Init();
    bool Update();
    bool CanQuit();
    void Quit();

    bool UpdateTileTextureUpload();
    bool UpdateTileTextureDownload();
    bool UpdateUninitializedTextures();
    bool UpdateTileEdit();

    bool InitLayers();
    bool InitTiles();
    bool InitMerge();
    bool InitPaint();

    // TODO change the ownership of the textures to slot_map instead
    bool CreateLayerTexture(const CanvasView& view, Layer layer);
    void DestroyLayerTexture(const CanvasView& view, Layer layer);

    Error CreateUninitializedTileTexture(const CanvasFile& file, Tile tile);
    Error CreateTileTexture(const CanvasFile& file, Tile tile);
    void DestroyTileTexture(const CanvasFile& file, Tile tile);
    bool MergeTileTextures(const CanvasFile& file, Tile overTile, Tile belowTile, MergeInfo info);

    std::optional<TileTextureUpload> ReserveTileTextureUpload(const CanvasFile& file, Tile tile);
    bool FreeTileTextureUpload(const CanvasFile& file, TileTextureUpload slot);
    bool CommitTileTextureUpload(const CanvasFile& file, TileTextureUpload slot);
    TileTextureUploadInfo& GetTileTextureUploadInfo(const CanvasFile& file, TileTextureUpload slot);

    std::optional<TileTextureDownload> DownloadTileTexture(const CanvasFile& file, Tile tile);
    bool QueryTileTextureDownload(const CanvasFile& file, TileTextureDownload slot);
    bool FreeTileTextureDownload(const CanvasFile& file, TileTextureDownload slot);
    TileTextureDownloadInfo& GetTileTextureDownloadInfo(const CanvasFile& file, TileTextureDownload slot);

    bool BeginCopyPass();
    SDL_GPUTexture* DuplicateTileTexture(const CanvasFile& file, Tile tile);
    SDL_GPUTexture* DuplicateTileTexture(SDL_GPUTexture* texture) const;
    bool EndCopyPass(bool wait);
    void CancelCopyPass();

    Error RenderCanvasView(const CanvasView& canvasView);
    Error ResizeCanvasView(const CanvasView& canvasView);
    Error PaintTiles(const CanvasFile& canvasFile, std::span<Tile> tiles, std::span<StrokePoint> points);
    Error EraseTiles(const CanvasFile& canvasFile, std::span<Tile> tiles, std::span<StrokePoint> points);

    // std::vector<SDL_GPUTexture*> DuplicateTileTextures(std::span<TileCoord> srcTiles);

    App* app;

    hash_set<Tile> tileToDraw{1024};
    hash_set<Layer> layerToDraw{1024};

    hash_map<Tile, SDL_GPUTexture*> tileTextures{2048};

    hash_set<Tile> tileTexturesToInitialize{1024};
    SDL_GPUFence* tileTexturesToInitializeFence;

    bool viewChanged = false;
    ViewportRenderData viewportRenderData;
    LayerRenderData layerRenderData;
    TileRenderData tileRenderData;

    // SDL3 GPU Renderer
    SDL_GPUShaderFormat shaderFormat;
    SDL_GPUDevice* device{};
    SDL_GPUTextureFormat textureFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUTextureFormat swapchainFormat = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    SDL_GPUTexture* canvasViewTexture{};
    std::vector<SDL_GPUTexture*> texturesToDelete;

    SDL_GPUCommandBuffer* currentCommandBuffer{};
    SDL_GPUCopyPass* currentCopyPass{};

    SDL_GPUGraphicsPipeline* layerGraphicsPipeline{};
    SDL_GPUSampler* layerSampler{};
    hash_map<Layer, SDL_GPUTexture*> layerTextures{1024};

    SDL_GPUGraphicsPipeline* tileGraphicsPipeline{};
    SDL_GPUSampler* tileSampler{};

    SDL_GPUTransferBuffer* tileUploadBuffer{};
    SDL_GPUTransferBuffer* tileBlankTextureBuffer{};
    std::byte* tileUploadBufferPtr{};
    std::array<TileTextureUploadInfo, TILE_MAX_CONCURENT_UPLOADS> tileUploadSlots;
    std::array<SDL_GPUFence*, TILE_MAX_INFLIGHT_UPLOADS> tileUploadSlotFences{};

    SDL_GPUTransferBuffer* tileDownloadBuffer{};
    std::byte* tileDownloadBufferPtr{};
    std::array<TileTextureDownloadInfo, TILE_MAX_CONCURENT_DOWNLOADS> tileDownloadSlots;
    std::array<SDL_GPUFence*, TILE_MAX_INFLIGHT_DOWNLOADS> tileDownloadSlotFences{};

    SDL_GPUComputePipeline* mergeComputePipeline{};

    SDL_GPUBuffer* paintStrokePointBuffer{};
    SDL_GPUComputePipeline* paintComputePipeline{};
    SDL_GPUComputePipeline* eraseComputePipeline{};
    SDL_GPUTransferBuffer* paintStrokePointTransferBuffer{};
    SDL_GPUTexture* brushTexture{};
    SDL_GPUSampler* brushSampler{};
    std::byte* paintStrokePointTransferBufferPtr{};
    SDL_GPUFence* tileEditFence{};
};
} // namespace Midori
