#pragma once

#include "colors.h"
#include "commands.h"
#include "defines.hpp"
#include "viewport.h"
#include "base.hpp"

#include <SDL3/SDL_gpu.h>
#include <array>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

namespace Midori {

class App;

class Canvas {
    // TODO: Use better state management (Finite State Machine or Hierarchical State Machine)

public:
    Canvas(const Canvas&) = delete;
    Canvas(Canvas&&) = delete;
    Canvas& operator=(const Canvas&) = delete;
    Canvas& operator=(Canvas&&) = delete;

    Canvas(App* app);
    ~Canvas() = default;

    void Update();
    void CullTiles(Viewport& viewport);

    void DeleteUpdate();

    // File Stuff
    bool CanQuit();
    bool Open();

    // Viewport Stuff
    void ViewUpdateState(glm::vec2 cursorPos);
    void ViewUpdateCursor(glm::vec2 cursorPos);
    bool viewZooming = false;
    bool viewPanning = false;
    bool viewRotating = false;
    glm::vec2 viewCursorStart = glm::vec2(0.0f);
    glm::vec2 viewCursorPrevious = glm::vec2(0.0f);
    Viewport viewport;
    CommandHistory viewCommands;
    std::unique_ptr<ViewportChangeCommand> currentViewportChangeCommand;

    [[nodiscard]] bool HasLayer(Layer layer) const;
    [[nodiscard]] bool LayerHasTile(Layer layer, Tile tile) const;
    [[nodiscard]] std::vector<Layer> Layers() const;
    [[nodiscard]] std::vector<Tile> LayerTiles(Layer layer) const;

    Layer CreateLayer(LayerInfo layerInfo);
    void DeleteLayer(Layer layer);
    bool SaveLayer(Layer layer);
    Layer DuplicateLayer(Layer layer, bool temporary = false);
    void MergeLayer(Layer overLayer, Layer belowLayer);
    bool SetLayerHeight(Layer layer, LayerHeight height);
    void CompactLayerHeight();

    [[nodiscard]] Tile GetLoadedTileAt(Layer layer, glm::ivec2 position) const;

    CommandHistory canvasCommands;

    Tile QueueLoadTile(Layer layer, glm::ivec2 position);
    void QueueUnloadTile(Layer layer, Tile tile);
    Tile CreateTile(Layer layer, glm::ivec2 position);
    bool QueueSaveTile(Layer layer, Tile tile);
    void QueueTileDelete(Layer layer, Tile tile);
    void MergeTiles(Tile overTile, Tile belowTile);

    App* app;

    LayerHeight layersCurrentMaxHeight = 0;
    std::unordered_map<Layer, LayerInfo> layerInfos;
    std::unordered_map<Layer, std::unordered_set<Tile>> layerTiles;
    std::unordered_map<Layer, std::unordered_map<glm::ivec2, Tile>> layerTilePos;
    std::unordered_set<Layer> layerToDelete;
    std::unordered_set<Layer> layersModified;
    std::vector<Layer> layersHeightSorted; // layer sorted by rendering order

    Layer selectedLayer = 0;
    Layer strokeLayer = 0;

    Layer layerLastAssigned = 0;
    std::vector<Layer> layersUnassigned;

    std::unordered_map<Tile, TileCoord> tileInfos;
    Tile tileLastAssigned = 0;
    std::vector<Tile> tilesUnassigned;
    std::unordered_set<Tile> tileToUnload;
    std::unordered_set<Tile> tileToDelete;
    std::unordered_map<Layer, std::unordered_set<glm::ivec2>> layerTilesSaved;
    std::unordered_map<Layer, std::unordered_set<Tile>> layerTilesModified;
    std::unordered_map<Layer, std::unordered_set<Tile>> allTileModified;

    std::string filename;

    bool strokeStarted = false;

    enum class TileReadState : std::uint8_t {
        Queued,
        Read,
        Decompressed,
        Uploaded,
    };

    struct TileTextureBuffer {
        std::array<uint8_t, TILE_SIZE> data;
    };
    Pool<TileTextureBuffer> tileTexturePool{32};

    struct TileReadStatus {
        Layer layer{};
        Tile tile{};
        TileReadState state{};
        TileTextureBuffer* encodedTexture{};
        size_t encodedSize{};
        TileTextureBuffer* rawTexture{};
    };
    std::unordered_map<Tile, TileReadStatus> tileReadQueue;
    void UpdateTileLoading();

    enum class TileWriteState : std::uint8_t {
        Queued,
        Downloading,
        Downloaded,
        Encoded,
        Written,
    };

    struct TileWriteStatus {
        Layer layer{};
        Tile tile{};
        TileWriteState state{};
        glm::ivec2 position{};
        TileTextureBuffer* encodedTexture{};
        size_t encodedSize{};
        TileTextureBuffer* rawTexture{};
    };
    std::unordered_map<Tile, TileWriteStatus> tileWriteQueue;
    void UpdateTileUnloading();

    struct StrokePoint {
        glm::vec4 color = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        glm::vec2 position;
        float radius = 8.0f;
        float flow = 0.5f;
        float hardness = 0.5f;
        float pad[3];
    };
    StrokePoint previousPoint = {};
    Arena<> strokeArena{4_MiB};
    std::vector<StrokePoint, arena_allocator<StrokePoint>> strokePoints{strokeArena};
    std::vector<Tile, arena_allocator<Tile>> allTileStrokeAffected{strokeArena};
    std::vector<Tile, arena_allocator<Tile>> strokeTileAffected;
    std::unique_ptr<TileModificationCommand> currentTileModificationCommand;
    std::vector<glm::ivec2, arena_allocator<glm::ivec2>> GetTilePosAffectedByStrokePoint(StrokePoint point);

    std::vector<Color> canvasTexture;
    glm::ivec2 canvasTextureSize;
    std::vector<Color> DownloadCanvasTexture(glm::ivec2& size) const;
    static bool SampleTexture(const std::vector<Color>& texture, glm::ivec2 textureSize, glm::vec2 pos, Color& color);

    // TODO: Merge brush and eraser in a single struct with a erase flag
    bool brushMode = true;
    bool eraserMode = false;

    // TODO: Move the Brush and Eraser to it's own struct and func

    struct BrushOptions {
        glm::vec4 color = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        bool opacityPressure = false;
        glm::vec2 opacityPressureRange = {0.0f, 1.0f};
        float flow = 0.5f;
        bool flowPressure = false;
        glm::vec2 flowPressureRange = {0.0f, 1.0f};
        float radius = 8.0f;
        bool radiusPressure = false;
        glm::vec2 radiusPressureRange = {0.0f, 1.0f};
        float hardness = 0.5f;
        bool hardnessPressure = false;
        glm::vec2 hardnessPressureRange = {0.0f, 1.0f};
        float spacing = 1.5f;
    } brushOptions;
    bool brushOptionsModified = false;
    void SaveBrush();
    void OpenBrush();

    [[nodiscard]] StrokePoint ApplyBrushPressure(StrokePoint point, float pressure) const;
    void StartBrushStroke(StrokePoint point);
    void UpdateBrushStroke(StrokePoint point);
    void EndBrushStroke(StrokePoint point);

    struct EraserOptions {
        float opacity = 1.0f;
        bool opacityPressure = false;
        glm::vec2 opacityPressureRange = {0.0f, 1.0f};
        float flow = 0.5f;
        bool flowPressure = false;
        glm::vec2 flowPressureRange = {0.0f, 1.0f};
        float radius = 8.0f;
        bool radiusPressure = false;
        glm::vec2 radiusPressureRange = {0.0f, 1.0f};
        float hardness = 0.5f;
        bool hardnessPressure = false;
        glm::vec2 hardnessPressureRange = {0.0f, 1.0f};
        float spacing = 1.5f;
    } eraserOptions;
    bool eraserOptionsModified = false;
    void SaveEraser();
    void OpenEraser();

    [[nodiscard]] StrokePoint ApplyEraserPressure(StrokePoint point, float pressure) const;
    void StartEraserStroke(StrokePoint point);
    void UpdateEraserStroke(StrokePoint point);
    void EndEraserStroke(StrokePoint point);

    void ChangeRadiusSize(glm::vec2 cursorDelta, bool slowMode);
};

} // namespace Midori
