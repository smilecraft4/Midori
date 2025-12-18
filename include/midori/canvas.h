#ifndef MIDORI_CANVAS_H
#define MIDORI_CANVAS_H

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

#include "midori/types.h"
#include "uuid.h"

namespace Midori {

class App;

class Canvas {
   public:
    struct View {
        glm::vec2 pan = glm::vec2(0.0f, 0.0f);
        float zoom_amount = 1.0f;
        glm::vec2 zoom_origin = glm::vec2(0.0f, 0.0f);
        float rotation = 0.0f;
        glm::vec2 rotate_start = glm::vec2(1.0f, 0.0f);
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

    // TODO: Move to it's own class
    // Viewport Stuff
    void ViewUpdateState(bool pan, bool zoom, bool rotate);
    void ViewUpdateCursor(glm::vec2 cursor_pos, glm::vec2 cursor_delta);
    void ViewPanStart();
    void ViewPan(glm::vec2 amount);
    void ViewPanStop();
    void ViewRotateStart(glm::vec2 start);
    void ViewRotate(float amount);
    void ViewRotateStop();
    void ViewZoomStart(glm::vec2 origin);
    void ViewZoom(float amount);
    void ViewZoomStop();
    static std::vector<glm::ivec2> GetViewVisibleTiles(const View &view, glm::vec2 size);

    [[nodiscard]] bool HasLayer(Layer layer) const;
    [[nodiscard]] bool LayerHasTile(Layer layer, Tile tile) const;
    [[nodiscard]] std::vector<Layer> Layers() const;
    [[nodiscard]] std::vector<Tile> LayerTiles(Layer layer) const;

    Layer CreateLayer(const std::string &name, std::uint8_t depth);
    void DeleteLayer(Layer layer);
    bool SaveLayer(Layer layer);
    void MergeLayer(Layer over_layer, Layer below_layer);
    bool SetLayerDepth(Layer layer, std::uint8_t depth);
    void CompactLayerHeight();

    [[nodiscard]] std::uint8_t GetLayerDepth(Layer layer) const;
    [[nodiscard]] Tile GetTileAt(Layer layer, glm::ivec2 position) const;

    struct HistoryTile {
        Layer layer;
        std::vector<glm::ivec2> pos;
        std::vector<SDL_GPUTexture *> textures;
    };
    static constexpr size_t history_capacity = 8;
    std::array<HistoryTile, history_capacity> history_stack = {};
    size_t history_pos = 0;
    size_t history_size = 0;
    size_t history_start = 0;

    void AddToHistory(HistoryTile history);
    void Undo();
    void Redo();

    // TODO: Change for a better name, this name implies that a new tile is
    // created and maybe saved, but This should not be saved
    // TODO: Use TilePos instead of Tile has a handle, and always operate using
    // the layer for Tile

    /**
     * @brief A list of errors of different severity returned by Tile functions
     *
     */

    /**
     * @brief Create a new blank tile. Only use if a tile is needed but LoadTile failed
     *
     * @param layer a handle to the layer of the tile
     * @param position Tile grid positino
     * @return std::expected<Tile, TileError>
     */
    std::expected<Tile, TileError> CreateTile(Layer layer, glm::ivec2 position);

    /**
     * @brief Load a tile at specified positon on specified layer.
     * This function may fail. It also can take some time before the
     * tile is fully loaded as it needs to read the file, decode, and
     * upload the tile texture to the gpu
     *
     * @param layer Layer of the tile
     * @param position canvas tile position where to load the tile
     * @return std::expected<Tile, TileError>
     */
    std::expected<Tile, TileError> LoadTile(Layer layer, glm::ivec2 position);

    /**
     * @brief Save a tile to disk, It will overwrite previous tile.
     * This operation can take some time 1~2 frame because it will
     * download the texture from the gpu
     *
     * @param tile Tile handle to save
     * @return std::optional<TileError> If a value is found this function failed
     */
    std::optional<TileError> SaveTile(Layer layer, Tile tile);

    /**
     * @brief Unload a specific tile
     *
     * @param tile
     */
    std::optional<TileError> UnloadTile(Layer layer, Tile tile);

    /**
     * @brief Will delete a tile from disk.
     *
     * @param tile Tile to delete
     */
    std::optional<TileError> DeleteTile(Layer layer, Tile tile);

    /**
     * @brief Merge a tile into another tile
     * The tile need to be on different layers
     *
     * @param over_tile
     * @param below_tile
     */
    void MergeTiles(Tile over_tile, Tile below_tile);

    /*
    void NewLayer(std::string name, uint8_t height);
    void DeleteLayer(uint8_t height);

    void SaveLayer(uint8_t height);

    void HideLayer(uint8_t height, bool hide);
    void LockLayer(uint8_t height, bool lock);
    void MoveLayer(uint8_t height, uint8_t new_height);
    void MergeLayerDown(uint8_t height);

    void NewTile(uint8_t height, glm::ivec2 pos);
    void DeleteTile(uint8_t height, glm::ivec2 pos);

    void OpenTile(uint8_t height, glm::ivec2 pos);
    void SaveTile(uint8_t height, glm::ivec2 pos);

    void LoadTile(uint8_t height, glm::ivec2 pos);
    void UnloadTile(uint8_t height, glm::ivec2 pos);

    void MergeTileDown(uint8_t height, glm::ivec2 pos);

    struct Tiles {
        size_t capacity;
        size_t count;

        std::vector<glm::ivec2> positions;
        std::vector<bool> modified;
        std::vector<bool> used;
        std::vector<bool> loaded;
        std::vector<bool> deleting;
        std::vector<bool> empty;

        Tiles(size_t capacity = 32);
        ~Tiles();

        void Resize(size_t capacity);
        size_t Alloc();
        size_t Free(size_t idx);
    };

    struct Layers {
        size_t capacity;
        size_t count;

        struct Info {
            std::string name;
            uint8_t opacity;
            uint8_t height;
        };
        std::vector<Info> infos;
        std::vector<bool> modified;
        std::vector<bool> deleted;
        std::vector<bool> used;
        std::vector<Tiles> tiles;

        Layers(size_t capacity = 32);
        ~Layers();

        void Resize(size_t capacity);

        size_t Alloc();
        size_t Free(size_t idx);
    };
    */
    // Member variables

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

    View view;
    glm::mat4 view_mat = glm::mat4(1.0f);
    std::vector<View> view_history;
    bool view_zooming = false;
    bool view_panning = false;
    bool view_rotating = false;

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

#endif
