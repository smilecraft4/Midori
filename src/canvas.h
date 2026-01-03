#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL_gpu.h>
#define UUID_SYSTEM_GENERATOR
#include <uuid.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

#include "color.h"
#include "history.h"
#include "types.h"
#include "viewport.h"

namespace Midori {

class App;

class PaintStrokeCommand : public Command {
   private:
    App &app_;
    Layer layer_;
    std::unordered_map<glm::ivec2, SDL_GPUTexture *> previousTileTextures_{};
    std::unordered_map<glm::ivec2, SDL_GPUTexture *> newTileTextures_{};

   public:
    explicit PaintStrokeCommand(App &app, Layer layer);
    virtual ~PaintStrokeCommand();

    void AddPreviousTileTexture(glm::ivec2 tile_pos, SDL_GPUTexture *previousTexture);
    void AddNewTileTexture(glm::ivec2 tile_pos, SDL_GPUTexture *newTexture);

    virtual std::string name() const;
    virtual void execute();
    virtual void revert();
};

class EraseStrokeCommand : public Command {
   private:
    App &app_;
    Layer layer_;
    std::unordered_map<glm::ivec2, SDL_GPUTexture *> previousTileTextures_;
    std::unordered_map<glm::ivec2, SDL_GPUTexture *> newTileTextures_;

   public:
    explicit EraseStrokeCommand(App &app, Layer layer);
    virtual ~EraseStrokeCommand();

    void AddPreviousTileTexture(glm::ivec2 tile_pos, SDL_GPUTexture *previousTexture);
    void AddNewTileTexture(glm::ivec2 tile_pos, SDL_GPUTexture *newTexture);

    virtual std::string name() const;
    virtual void execute();
    virtual void revert();
};

class Canvas {
   public:
    struct View {
        glm::vec2 pan = glm::vec2(0.0f, 0.0f);
        glm::vec2 zoom_amount = glm::vec2(1.0f, 1.0f);
        glm::vec2 zoom_origin = glm::vec2(0.0f, 0.0f);
        float rotation = 0.0f;
        glm::vec2 rotate_start = glm::vec2(1.0f, 0.0f);
        bool flippedH = false;
        bool flippedV = false;
    };

    Canvas(const Canvas &) = delete;
    Canvas(Canvas &&) = delete;
    Canvas &operator=(const Canvas &) = delete;
    Canvas &operator=(Canvas &&) = delete;

    Canvas(App *app);
    ~Canvas() = default;

    void Update();
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

    [[nodiscard]] bool HasLayer(Layer layer) const;
    [[nodiscard]] bool LayerHasTile(Layer layer, Tile tile) const;
    [[nodiscard]] std::vector<Layer> Layers() const;
    [[nodiscard]] std::vector<Tile> LayerTiles(Layer layer) const;

    Layer CreateLayer(const std::string &name, std::uint8_t depth);
    void DeleteLayer(Layer layer);
    bool SaveLayer(Layer layer);
    Layer DuplicateLayer(Layer layer, bool temporary = false);
    void MergeLayer(Layer over_layer, Layer below_layer);
    bool SetLayerDepth(Layer layer, std::uint8_t depth);
    void CompactLayerHeight();

    [[nodiscard]] std::uint8_t GetLayerDepth(Layer layer) const;
    [[nodiscard]] Tile GetTileAt(Layer layer, glm::ivec2 position) const;

    Viewport viewport;
    static constexpr size_t VIEW_HISTORY_MAX_SIZE = 2048;
    HistoryTree viewHistory;

    static constexpr size_t HISTORY_MAX_SIZE = 128;
    std::unordered_map<glm::ivec2, SDL_GPUTexture *> eraseStrokeDuplicatedTextures;
    HistoryTree canvasHistory;
    std::unique_ptr<ViewportChangeCommand> currentViewportChangeCommand;

    std::expected<Tile, TileError> LoadTile(Layer layer, glm::ivec2 position);
    std::optional<TileError> UnloadTile(Layer layer, Tile tile);
    std::expected<Tile, TileError> CreateTile(Layer layer, glm::ivec2 position);
    std::optional<TileError> SaveTile(Layer layer, Tile tile);
    std::optional<TileError> DeleteTile(Layer layer, Tile tile);
    void MergeTiles(Tile over_tile, Tile below_tile);

    App *app;

    std::uint8_t current_max_layer_height = 0;
    std::unordered_map<Layer, LayerInfo> layer_infos;
    std::unordered_map<Layer, std::unordered_set<Tile>> layer_tiles;
    std::unordered_map<Layer, std::unordered_map<glm::ivec2, Tile>> layer_tile_pos;
    std::unordered_set<Layer> layer_to_delete;
    std::unordered_set<Layer> layersModified;

    Layer selected_layer = 0;
    Layer stroke_layer = 0;

    Layer last_assigned_layer = 0;
    std::vector<Layer> unassigned_layers;

    std::unordered_map<Tile, TileInfo> tile_infos;
    Tile last_assigned_tile = 0;
    std::vector<Tile> unassigned_tiles;
    std::unordered_set<Tile> tile_to_unload;
    std::unordered_set<Tile> tile_to_delete;
    std::unordered_map<Layer, std::unordered_set<glm::ivec2>> layerTilesSaved;
    std::unordered_map<Layer, std::unordered_set<Tile>> layerTilesModified;
    std::unordered_map<Layer, std::unordered_set<Tile>> allTileModified;

    std::string filename;

    bool stroke_started = false;

    enum class TileReadState : std::uint8_t {
        Queued,
        Read,
        Decompressed,
        Uploaded,
    };

    struct TileReadStatus {
        uuids::uuid layer_id;
        Tile tile;
        TileReadState state;
        std::vector<std::uint8_t> encoded_texture;
        std::vector<std::uint8_t> raw_texture;
    };
    std::unordered_map<Tile, TileReadStatus> tile_read_queue;
    void UpdateTileLoading();

    enum class TileWriteState : std::uint8_t {
        Queued,
        Downloading,
        Downloaded,
        Encoded,
        Written,
    };

    struct TileWriteStatus {
        uuids::uuid layer_id;
        Tile tile;
        TileWriteState state;
        glm::ivec2 position;
        std::vector<std::uint8_t> encoded_texture;
        std::vector<std::uint8_t> raw_texture;
    };
    std::unordered_map<Tile, TileWriteStatus> tile_write_queue;
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
    std::vector<StrokePoint> stroke_points;
    std::unordered_set<Tile> stroke_tile_affected;
    std::unordered_set<Tile> allTileStrokeAffected;
    std::unordered_map<glm::ivec2, SDL_GPUTexture *> eraseStrokePreviousTextures;
    StrokePoint previous_point = {};

    bool brush_mode = true;
    bool eraser_mode = false;

    struct BrushOptions {
        glm::vec4 color = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        bool opacity_pressure = false;
        glm::vec2 opacity_pressure_range = {0.0f, 1.0f};

        float flow = 0.5f;
        bool flow_pressure = false;
        glm::vec2 flow_pressure_range = {0.0f, 1.0f};

        float radius = 8.0f;
        bool radius_pressure = false;
        glm::vec2 radius_pressure_range = {0.0f, 1.0f};

        float hardness = 0.5f;
        bool hardness_pressure = false;
        glm::vec2 hardness_pressure_range = {0.0f, 1.0f};

        float spacing = 1.5f;
    } brush_options;
    bool brush_options_modified = false;
    [[nodiscard]] StrokePoint ApplyBrushPressure(StrokePoint point, float pressure) const;
    void StartBrushStroke(StrokePoint point);
    void UpdateBrushStroke(StrokePoint point);
    void EndBrushStroke(StrokePoint point);
    void SaveBrush();
    void OpenBrush();

    struct EraserOptions {
        float opacity = 1.0f;
        bool opacity_pressure = false;
        glm::vec2 opacity_pressure_range = {0.0f, 1.0f};

        float flow = 0.5f;
        bool flow_pressure = false;
        glm::vec2 flow_pressure_range = {0.0f, 1.0f};

        float radius = 8.0f;
        bool radius_pressure = false;
        glm::vec2 radius_pressure_range = {0.0f, 1.0f};

        float hardness = 0.5f;
        bool hardness_pressure = false;
        glm::vec2 hardness_pressure_range = {0.0f, 1.0f};

        float spacing = 1.5f;
    } eraser_options;
    bool eraser_options_modified = false;
    [[nodiscard]] StrokePoint ApplyEraserPressure(StrokePoint point, float pressure) const;
    void StartEraserStroke(StrokePoint point);
    void UpdateEraserStroke(StrokePoint point);
    void EndEraserStroke(StrokePoint point);
    void SaveEraser();
    void OpenEraser();

    void ChangeRadiusSize(glm::vec2 cursorDelta, bool slowMode);

    /// @brief Will download the canvas texture (RGBA8 linear srgb premultiplied) immediatly (may freeze)
    std::vector<Color> DownloadCanvasTexture(glm::ivec2 &size);

    /// @brief sample the RGBA8 at texture coordinates with nearest neighbour blending is activated (the color is linear
    /// srgb premultiplied), THERE MUST BE a valid canvasTexture downloaded beforehand or this function will fail, to
    /// download the canvasTexture call `DownloadCanvasTexture()`
    /// @param pos [-1, 1]
    bool SampleTexture(const std::vector<Color> &texture, glm::ivec2 textureSize, glm::vec2 pos, Color &color);

    std::vector<Color> canvasTexture_;
    glm::ivec2 canvasTextureSize_;
};
}  // namespace Midori

/*
struct File {
    char id[4]; // file extension name only using 2 char tho
    uint8_t fileVersion[3]; //https://semver.org/
    uint64_t fileSize; // keep in track the size of the file

    struct Layer {
        UUID uuid;  // unique id for this layer used to find tile layer in file
        char *name;
        uint8_t height;
        uint8_t blendMode;
        bool locked;
        bool hidden;
        uint64_t tileCount;
    };
    uint8_t layerCount;
    Layer layers[UINT8_MAX]; // This is always added to disk too avoid rewrite when deleting or adding a new layer

    // malloc like allocator
    struct Tile {
        UUID layerUuid // find a way to create a true UUID for a layer that linked to a layer
        int32_t x;
        int32_t y;
        uint64_t encodedPos; // compressed as .qoi file
        uint16_t encodedSize;
    };
    Tile *tiles;

    uint8_t* tilesData; // store the data of the file. This is not loaded in ram and kept on disk (this is the large bit
>3Gb)
    // if no slot are available just append from this place and do not override the Slot structure

    // get's rewritten everytime no large enough
    struct Slot {
        size_t *pos;
        size_t *size;
    };
    size_t slotCount;
    Slot *slots;
};
*/