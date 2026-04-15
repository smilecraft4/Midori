#pragma once

#include "colors.h"
#include "commands.h"
#include "viewport.h"
#include <EASTL/unordered_map.h>
#include <EASTL/unordered_set.h>
#include <SDL3/SDL_gpu.h>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

namespace Midori {

class App;

class Canvas {
    // TODO: Use better state management (Finite State Machine or Hierarchical State Machine)

public:
    // struct View {
    //     glm::vec2 pan = glm::vec2(0.0f, 0.0f);
    //     glm::vec2 zoomAmount = glm::vec2(1.0f, 1.0f);
    //     glm::vec2 zoomOrigin = glm::vec2(0.0f, 0.0f);
    //     float rotation = 0.0f;
    //     glm::vec2 rotateStart = glm::vec2(1.0f, 0.0f);
    //     bool flippedH = false;
    //     bool flippedV = false;
    // };

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
    void ViewUpdateState(glm::vec2 cursor_pos);
    void ViewUpdateCursor(glm::vec2 cursor_pos);
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
    [[nodiscard]] eastl::vector<Layer> Layers() const;
    [[nodiscard]] eastl::vector<Tile> LayerTiles(Layer layer) const;

    Layer CreateLayer(LayerInfo layerInfo);
    void DeleteLayer(Layer layer);
    bool SaveLayer(Layer layer);
    Layer DuplicateLayer(Layer layer, bool temporary = false);
    void MergeLayer(Layer over_layer, Layer below_layer);
    bool SetLayerHeight(Layer layer, LayerHeight height);
    void CompactLayerHeight();

    [[nodiscard]] Tile GetLoadedTileAt(Layer layer, glm::ivec2 position) const;

    CommandHistory canvasCommands;

    Tile QueueLoadTile(Layer layer, glm::ivec2 position);
    void QueueUnloadTile(Layer layer, Tile tile);
    Tile CreateTile(Layer layer, glm::ivec2 position);
    bool QueueSaveTile(Layer layer, Tile tile);
    void QueueTileDelete(Layer layer, Tile tile);
    void MergeTiles(Tile over_tile, Tile below_tile);

    App* app;

    LayerHeight layersCurrentMaxHeight = 0;
    eastl::unordered_map<Layer, LayerInfo> layerInfos;
    eastl::unordered_map<Layer, eastl::unordered_set<Tile>> layerTiles;
    eastl::unordered_map<Layer, eastl::unordered_map<glm::ivec2, Tile>> layerTilePos;
    eastl::unordered_set<Layer> layerToDelete;
    eastl::unordered_set<Layer> layersModified;

    // eastl::unordered_map<Layer, LayerInfo> layersInfo; // store data such as height, name, opacity, blendMode, etc...
    // eastl::vector<Layer> layersReusable;               // layers that are available before increasing layerLastIssued;
    eastl::vector<Layer> layersHeightSorted;           // layer sorted by rendering order
    // std::atomic<Layer> layerLastIssued;                // last layer issued, check layersReusable before

    Layer selectedLayer = 0;
    Layer strokeLayer = 0;

    Layer layerLastAssigned = 0;
    eastl::vector<Layer> layersUnassigned;

    // Move to a Tile Structs
    eastl::unordered_map<Tile, TileCoord> tileInfos;
    Tile tileLastAssigned = 0;
    eastl::vector<Tile> tilesUnassigned;
    eastl::unordered_set<Tile> tileToUnload;
    eastl::unordered_set<Tile> tileToDelete;
    eastl::unordered_map<Layer, eastl::unordered_set<glm::ivec2>> layerTilesSaved;
    eastl::unordered_map<Layer, eastl::unordered_set<Tile>> layerTilesModified;
    eastl::unordered_map<Layer, eastl::unordered_set<Tile>> allTileModified;

    std::string filename;

    bool stroke_started = false;

    enum class TileReadState : std::uint8_t {
        Queued,
        Read,
        Decompressed,
        Uploaded,
    };

    struct TileReadStatus {
        Layer layer;
        Tile tile;
        TileReadState state;
        eastl::vector<std::uint8_t> encodedTexture;
        eastl::vector<std::uint8_t> rawTexture;
    };
    eastl::unordered_map<Tile, TileReadStatus> tile_read_queue;
    void UpdateTileLoading();

    enum class TileWriteState : std::uint8_t {
        Queued,
        Downloading,
        Downloaded,
        Encoded,
        Written,
    };

    struct TileWriteStatus {
        Layer layer;
        Tile tile;
        TileWriteState state;
        glm::ivec2 position;
        eastl::vector<std::uint8_t> encodedTexture;
        eastl::vector<std::uint8_t> rawTexture;
    };
    eastl::unordered_map<Tile, TileWriteStatus> tile_write_queue;
    void UpdateTileUnloading();

    struct StrokePoint {
        glm::vec4 color = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        glm::vec2 position;
        float radius = 8.0f;
        float flow = 0.5f;
        float hardness = 0.5f;
        float _pad0;
        float _pad1;
        float _pad2;
    };
    eastl::vector<StrokePoint> stroke_points;
    eastl::unordered_set<Tile> stroke_tile_affected;
    eastl::hash_set<Tile> allTileStrokeAffected;
    StrokePoint previous_point = {};
    std::unique_ptr<TileModificationCommand> currentTileModificationCommand;

    eastl::vector<Color> DownloadCanvasTexture(glm::ivec2& size);
    bool SampleTexture(const eastl::vector<Color>& texture, glm::ivec2 textureSize, glm::vec2 pos, Color& color);
    eastl::vector<Color> canvasTexture_;
    glm::ivec2 canvasTextureSize_;

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
        bool hardness_pressure = false;
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
        bool hardness_pressure = false;
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
