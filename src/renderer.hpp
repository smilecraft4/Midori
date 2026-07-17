#pragma once

#include "defines.hpp"
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

struct Renderer {
    using TileTextureDownload = size_t;
    using TileTextureUpload = size_t;

    enum class Error : uint8_t {
        None,

        MissingTexture,
        UploadSlotMissing,
        DownloadSlotMissing,
        OutOfTileMemory,
        OutOfLayerMemory,
        OutOfMemory,

        Unknown,
    };

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

    struct TileTextureDownloadInfo {
        std::atomic<TextureDownloadState> state{TextureDownloadState::None};
        TileCoord tile;
        std::byte* data{};
        size_t fence;
    };

    struct TileTextureUploadInfo {
        std::atomic<TextureUploadState> state{TextureUploadState::None};
        TileCoord tile;
        std::byte* data{};
        size_t fence;
    };

    struct MergeRenderData {
        std::uint32_t srcBlendMode;
        float srcOpacity;
        glm::ivec2 srcPos;
        glm::ivec2 srcSize;
        glm::ivec2 dstPos;
        glm::ivec2 dstSize;
        uint8_t pad[8];
    };

    struct StrokeRenderData {
        std::uint32_t pointsNum = 0;
    };

    Renderer(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    Renderer(App* app);
    ~Renderer() = default;

    bool Init();
    bool Update();
    bool Render();
    bool Resize();
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

    bool CreateLayerTexture(Layer layer);
    void DestroyLayerTexture(Layer layer);

    Error CreateUninitializedTileTexture(TileCoord tile);
    Error CreateTileTexture(TileCoord tile);
    void DestroyTileTexture(TileCoord tile);
    bool MergeTileTextures(TileCoord overTile, TileCoord belowTile);

    std::optional<TileTextureUpload> ReserveTileTextureUpload(TileCoord tile);
    bool FreeTileTextureUpload(TileTextureUpload slot);
    bool CommitTileTextureUpload(TileTextureUpload slot);
    TileTextureUploadInfo& GetTileTextureUploadInfo(TileTextureUpload slot);

    std::optional<TileTextureDownload> DownloadTileTexture(TileCoord tile);
    bool QueryTileTextureDownload(TileTextureDownload slot);
    bool FreeTileTextureDownload(TileTextureDownload slot);
    TileTextureDownloadInfo& GetTileTextureDownloadInfo(TileTextureDownload slot);

    bool BeginCopyPass();
    SDL_GPUTexture* DuplicateTileTexture(TileCoord tile);
    SDL_GPUTexture* DuplicateTileTexture(SDL_GPUTexture* texture) const;
    bool EndCopyPass(bool wait);
    void CancelCopyPass();

    // std::vector<SDL_GPUTexture*> DuplicateTileTextures(std::span<TileCoord> srcTiles);

    App* app;

    hash_set<TileCoord> tileToDraw{1024};
    hash_set<Layer> layerToDraw{1024};

    hash_map<TileCoord, SDL_GPUTexture*> tileTextures{2048};

    hash_set<TileCoord> tileTexturesToInitialize{1024};
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
    SDL_GPUTexture* canvasTexture{};
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
