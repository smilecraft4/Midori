#pragma once

#include "layers.h"
#include "tiles.h"

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace Midori {

class App;

class Renderer {
public:
    Renderer(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    Renderer(App* app);
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

    TileTextureError CreateTileTexture(Tile tile);
    TileTextureError UploadTileTexture(Tile tile, std::span<uint8_t, TILE_SIZE> pixels);

    void ReleaseTileTexture(Tile tile);
    bool MergeTileTextures(Tile overTile, Tile belowTile);

    App* app;

    std::unordered_set<Tile> tileToDraw;
    std::unordered_set<Layer> layerToDraw;

    // Common data
    SDL_GPUDevice* device = nullptr;
    SDL_GPUTextureFormat textureFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUTextureFormat swapchainFormat = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    SDL_GPUTexture* canvasTexture = nullptr;
    std::vector<SDL_GPUTexture*> texturesToDelete;

    struct ViewportRenderData {
        glm::mat4 projection = glm::mat4(1.0f);
        glm::mat4 view = glm::mat4(1.0f);
    } viewportRenderData;
    bool viewChanged = false;

    // Layer data
    struct LayerRenderData {
        float opacity = 1.0f;
        std::uint32_t blendMode = (uint32_t)BlendMode::Alpha;
    } layerRenderData;

    SDL_GPUShader* layerVertexShader = nullptr;
    SDL_GPUShader* layerFragmentShader = nullptr;
    SDL_GPUGraphicsPipeline* layerGraphicsPipeline = nullptr;
    SDL_GPUSampler* layerSampler = nullptr;
    std::unordered_map<Layer, SDL_GPUTexture*> layerTextures;
    size_t lastLayerRenderedNum = 0;

    // Tile data
    struct TileRenderData {
        glm::vec2 position = glm::vec2(0.0f, 0.0f);
        glm::vec2 size = glm::vec2(TILE_WIDTH, TILE_HEIGHT);
    } tileRenderData;

    SDL_GPUShader* tileVertexShader = nullptr;
    SDL_GPUShader* tileFragmentShader = nullptr;
    SDL_GPUGraphicsPipeline* tileGraphicsPipeline = nullptr;
    SDL_GPUSampler* tileSampler = nullptr;
    std::unordered_map<Tile, SDL_GPUTexture*> tileTextures;
    std::unordered_set<Tile> tileTextureUninitialized;
    size_t lastRenderedTilesNum = 0;

    // Tile upload
    SDL_GPUTransferBuffer* tileUploadBuffer = nullptr;
    SDL_GPUTransferBuffer* tileBlankTextureBuffer = nullptr;
    uint8_t* tileUploadBufferPtr = nullptr;
    std::unordered_map<Tile, size_t> allocatedTileUploadOffset;
    std::vector<size_t> freeTileUploadOffset;

    // Tile download

    bool DownloadTileTexture(Tile tile);
    [[nodiscard]] bool IsTileTextureDownloaded(Tile tile) const;
    bool CopyTileTextureDownloaded(Tile tile, std::span<uint8_t, TILE_SIZE> tileTexture);
    SDL_GPUTexture* DuplicateTileTexture(SDL_GPUCopyPass* copyPass, SDL_GPUTexture* tileTexture) const;

    SDL_GPUTransferBuffer* tileDownloadBuffer = nullptr;
    uint8_t* tileDownloadBufferPtr = nullptr;
    std::unordered_map<Tile, size_t> allocatedTileDownloadOffset;
    std::unordered_set<Tile> tileDownloaded;
    std::vector<size_t> freeTileDownloadOffset;
    SDL_GPUFence* tileDownloadFence = nullptr; // TODO: Use multiple fences

    // OPERATIONS

    // Texture merging
    bool InitMerge();
    struct MergeRenderData {
        std::uint32_t srcBlendMode;
        float srcOpacity;
        glm::ivec2 srcPos;
        glm::ivec2 srcSize;

        std::uint32_t dstBlendMode;
        float dstOpacity;
        glm::ivec2 dstPos;
        glm::ivec2 dstSize;
    };
    SDL_GPUComputePipeline* mergeComputePipeline = nullptr;

    bool InitPaint();
    struct StrokeRenderData {
        std::uint32_t pointsNum = 0;
    };
    static constexpr size_t MAX_PAINT_STROKE_POINTS = 2048;
    SDL_GPUBuffer* paintStrokePointBuffer = nullptr;
    SDL_GPUComputePipeline* paintComputePipeline = nullptr;
    SDL_GPUComputePipeline* eraseComputePipeline = nullptr;
    SDL_GPUTransferBuffer* paintStrokePointTransferBuffer = nullptr;
    SDL_GPUTexture* brushTexture = nullptr;
    SDL_GPUSampler* brushSampler = nullptr;
    std::uint8_t* paintStrokePointTransferBufferPtr = nullptr;

    SDL_GPUShaderFormat shaderFormat;
};
} // namespace Midori
