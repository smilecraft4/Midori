#pragma once

#include "base.hpp"
#include "defines.hpp"
#include "errors.hpp"
#include "tiles.hpp"
#include "colors.hpp"

#include <SDL3/SDL_gpu.h>
#include <bitset>
#include <cstddef>
#include <expected>
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
    enum class Operation : uint8_t {
        New,
        Duplicate,
        Move,
        Merge,
        Paint,
        Erase,
        Destroy,
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

    struct MergeInfo {
        BlendMode blendMode;
        float opacity;
    };

    enum class StagingBufferState : uint8_t {
        None,
        Completed,
        Transfering,
        Done,
    };

    template<size_t Capacity = 0>
    struct StagingBuffer {
        SDL_GPUTransferBuffer* buffer{};
        std::byte* data{};
        SDL_GPUFence* fence{};

        std::mutex mutex;
        StagingBufferState state{};
        std::bitset<Capacity> used{};
        std::bitset<Capacity> commited{};
        TileTransfer::ID ids[Capacity]{};
    };

    Renderer(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    Renderer(App* app);
    ~Renderer() = default;

    bool Init();
    bool Update();
    bool CanQuit() const;
    void Quit();
    Error Render();

    Error UpdateTileTextureUpload();
    Error UpdateTileTextureDownload();
    bool UpdateUninitializedTextures();
    bool UpdateTileEdit();

    bool InitLayers();
    bool InitTiles();
    bool InitMerge();
    bool InitPaint();

    // TODO change the ownership of the textures to slot_map instead
    bool CreateLayerTexture(const CanvasView& view, Layer layer);
    void DestroyLayerTexture(const CanvasView& view, Layer layer);

    Error CreateUninitializedTileTexture(Tile tile);
    Error CreateTileTexture(Tile tile);
    void DestroyTileTexture(Tile tile);
    bool MergeTileTextures(Tile overTile, Tile belowTile, MergeInfo info);

    bool BeginCopyPass();
    SDL_GPUTexture* DuplicateTileTexture(Tile tile);
    SDL_GPUTexture* DuplicateTileTexture(SDL_GPUTexture* texture) const;
    bool EndCopyPass(bool wait);
    void CancelCopyPass();

    Error RenderCanvasView(CanvasView& canvasView);
    Error ResizeCanvasView(const CanvasView& canvasView);

    Error MergeTiles(const CanvasFile& canvasFile, std::span<Tile> srcTiles, std::span<Tile> dstTiles, MergeInfo info);
    Error PaintTiles(const CanvasFile& canvasFile, std::span<Tile> tiles, std::span<StrokePoint> points);
    Error EraseTiles(const CanvasFile& canvasFile, std::span<Tile> tiles, std::span<StrokePoint> points);

    std::expected<TileStageBufferSlot, Error> ReserveTileUploadSlot(TileTransfer::ID id);
    std::expected<TileStageBufferSlot, Error> GetTileUploadSlot(TileTransfer::ID id);
    void* GetTileUploadSlotData(TileStageBufferSlot slot);
    void CommitTileUploadSlot(TileStageBufferSlot slot);
    void FreeTileUploadSlot(TileStageBufferSlot slot);

    std::expected<TileStageBufferSlot, Error> QueueTileDownloadSlot(TileTransfer::ID id);
    std::expected<TileStageBufferSlot, Error> GetTileDownloadSlot(TileTransfer::ID id);
    void* GetTileDownloadSlotData(TileStageBufferSlot slot);
    void FreeTileDownloadSlot(TileStageBufferSlot slot);

    App* app;
    arena<> arena{1_MiB};

    StagingBuffer<TILE_UPLOAD_BUFFER_CAPACITY> tileUploadStagingBuffers[TILE_MAX_UPLOAD_BUFFER];
    StagingBuffer<TILE_DOWNLOAD_BUFFER_CAPACITY> tileDownloadStagingBuffers[TILE_MAX_DOWNLOAD_BUFFERS];

    Operation currentOperation;

    hash_set<Tile> tileToDraw{1024};
    hash_set<Layer> layerToDraw{1024};

    hash_map<Tile, SDL_GPUTexture*, arena_allocator<Tile>> tileTextures{2048, arena};

    hash_set<Tile, arena_allocator<Tile>> tileTexturesToInitialize{1024, arena};
    SDL_GPUFence* tileTexturesToInitializeFence;

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
    hash_map<Layer, SDL_GPUTexture*, arena_allocator<Layer>> layerTextures{1024, arena};

    SDL_GPUGraphicsPipeline* tileGraphicsPipeline{};
    SDL_GPUSampler* tileSampler{};

    SDL_GPUTransferBuffer* tileBlankTextureBuffer{};

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
