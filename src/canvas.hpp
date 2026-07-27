#pragma once

#include "brushes.hpp"
#include "colors.hpp"
#include "commands.hpp"
#include "defines.hpp"
#include "errors.hpp"
#include "layers.hpp"
#include "renderer.hpp"
#include "states.hpp"
#include "tiles.hpp"
#include "viewport.hpp"
#include "base.hpp"

#include "../deps/liblmdb/lmdb.h"
#include <SDL3/SDL_gpu.h>
#include <array>
#include <expected>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <filesystem>
#include <optional>

namespace Midori {

struct App;

struct TileWriteData {
    Tile tile;
    TileWriteState state;
    Renderer::TileTextureDownload rendererSlot;
};

struct TileReadData {
    Tile tile;
    TileReadState state;
    Renderer::TileTextureUpload rendererSlot;
};

struct Canvas {
    Canvas(const Canvas&) = delete;
    Canvas(Canvas&&) = delete;
    Canvas& operator=(const Canvas&) = delete;
    Canvas& operator=(Canvas&&) = delete;

    Canvas(App* app);
    ~Canvas() = default;

    void Update();
    void UpdateTileVisibility(Viewport& viewport);
    bool UpdateTileReading();
    bool UpdateTileWriting();

    [[nodiscard]] bool CanQuit() const;
    bool Open();

    Layer CreateLayer(LayerCreateInfo createInfo);
    Layer CreateLayer(LayerCreateInfo createInfo, Layer id);
    bool DeleteLayer(Layer layer);
    bool SaveLayer(Layer layer);
    Layer DuplicateLayer(Layer srcLayer, bool temporary = false);
    bool MergeLayers(Layer srcLayer, Layer dstLayer);
    bool MoveLayer(Layer layer, LayerHeight newHeight);

    bool LoadTile(Tile tile);
    bool UnloadTile(Tile tile);
    bool CreateTile(Tile tile, bool initialize = true);
    bool DestroyTile(Tile tile);
    bool SaveTile(Tile tile);
    bool DeleteTile(Tile tile);
    bool MergeTiles(Tile srcTile, Tile dstTile);
    bool MoveTile(Tile srcTile, Tile dstTile);
    void MarkTileDirty(Tile tile);

    // Those should be moved to the state directly instead
    // They indeed are pretty tighly coupled with the renderer
    // and it this is a fair concern
    void SavePaintBrush();
    void OpenPaintBrush();
    void StartPaintStroke(StrokePoint point);
    void UpdatePaintStroke(StrokePoint point);
    void EndPaintStroke(StrokePoint point);

    void SaveEraseBrush();
    void OpenEraseBrush();
    void StartEraseStroke(StrokePoint point);
    void UpdateEraseStroke(StrokePoint point);
    void EndEraseStroke(StrokePoint point);

    static std::vector<glm::ivec2, arena_allocator<glm::ivec2>> GetTilePosAffectedByStrokePoint(StrokePoint point,
                                                                                                arena<>& arena);

    App* app;
    std::string filename;

    arena<> strokeArena{4_MiB};

    Viewport viewport;
    CommandHistory viewCommands;
    std::optional<ViewportChangeCommand> currentViewportChangeCommand;

    CommandHistory canvasCommands;
    std::optional<TileModificationCommand> currentTileModificationCommand;

    slot_map<Layer, LayerInfo> layers{1024};
    std::vector<Layer> layersTree;

    hash_set<Tile> tilesLoaded{1024};
    hash_set<Tile> tilesModified{1024};
    hash_map<Tile, TileWriteData> tilesWriting{1024};
    hash_map<Tile, TileReadData> tilesReading{1024};

    Layer selectedLayer = LAYER_INVALID;
    Layer strokeLayer = LAYER_INVALID;

    Tool currentTool{Tool::Paint};
    ToolState currentToolState{ToolState::None};

    StrokePoint previousPoint{};
    std::vector<StrokePoint, arena_allocator<StrokePoint>> strokePoints{strokeArena};
    hash_set<Tile, arena_allocator<Tile>> strokeTileAffected{strokeArena};

    std::vector<Color> canvasTexture;
    glm::ivec2 canvasTextureSize;
    std::vector<Color> DownloadCanvasTexture(glm::ivec2& size) const;
    static bool SampleTexture(const std::vector<Color>& texture, glm::ivec2 textureSize, glm::vec2 pos, Color& color);
};

struct CanvasFile {
    struct Version {
        uint16_t major;
        uint16_t minor;
        uint16_t patch;
    };

    CanvasFile(const CanvasFile&) = delete;
    CanvasFile(CanvasFile&&) noexcept = delete;
    CanvasFile& operator=(const CanvasFile&) = delete;
    CanvasFile& operator=(CanvasFile&&) noexcept = delete;

    explicit CanvasFile(App* app);
    ~CanvasFile();

    Error Init(size_t newId);
    Error New();
    Error SaveAs(std::filesystem::path newFilename);
    Error Open(std::filesystem::path newFilename);
    Error Save();
    Error Close();
    Error Update();
    Error Quit();
    void SyncCanvasViews();

    [[nodiscard]] bool HasLayer(Layer layer) const;
    std::expected<Layer, Error> NewLayer(LayerCreateInfo createInfo);
    std::expected<Layer, Error> GetLayer(LayerHeight height);
    Error DeleteLayer(Layer layer);
    Error SetLayerName(Layer layer, std::string newName);
    Error SetLayerHeight(Layer layer, LayerHeight newHeight);
    Error SetBlendMode(Layer layer, BlendMode newBlendMode);
    Error MergeLayers(Layer srcLayer, Layer dstLayer);

    std::expected<Layer, Error> DuplicateLayer(Layer layer);
    [[nodiscard]] bool HasTile(Tile tile) const;
    Error NewTile(Tile tile);
    Error DeleteTile(Tile tile);
    Error DestroyTile(Tile tile);
    Error MarkTileDirty(Tile tile);
    Error ReadTile(Tile tile);
    Error WriteTile(Tile tile);
    Error MergeTiles(Tile srcTile, Tile dstTile);
    Error MoveTile(Tile srcTile, Tile dstTile);
    Error DuplicateTile(Tile srcTile, Tile dstTile);

    App* app;
    size_t id;
    Version version{.major = 0, .minor = 1, .patch = 0}; // TODO move to defines.hpp
    std::optional<std::filesystem::path> filename;
    std::array<std::optional<size_t>, CANVASVIEW_MAX> canvasViewIds{};

    arena<> arena{16_KiB};
    slot_map<Layer, LayerInfo> layers{1024};
    std::vector<Layer> layersTree;
    hash_set<Tile> tilesLoaded{1024};
    hash_set<Tile> tilesModified{1024};
    hash_map<Tile, TileWriteData> tilesWriting{1024};
    hash_map<Tile, TileReadData> tilesReading{1024};

    CommandHistory canvasHistory{512};
    std::optional<TileModificationCommand> currentTileCommand;

    MDB_env* env{};
    MDB_dbi dbiMetadata;
    MDB_dbi dbiLayers;
    MDB_dbi dbiTiles;
};

struct CanvasView {
    CanvasView(const CanvasView&) = delete;
    CanvasView(CanvasView&&) noexcept = delete;
    CanvasView& operator=(const CanvasView&) = delete;
    CanvasView& operator=(CanvasView&&) noexcept = delete;

    CanvasView(App* app, size_t windowId, size_t canvasFileId);
    ~CanvasView();

    Error Init(size_t newId);
    Error Update();
    Error Quit();
    Error Resize(int newWidth, int newHeight);
    Error Move(int newX, int newY);

    Error SelectLayer(Layer layer);
    Error DeselectedLayer(Layer layer);

    Error LoadTile(Tile tile);
    Error UnloadTile(Tile tile);

    App* app;
    size_t id;
    size_t windowId;
    size_t canvasFileId;
    int x{};
    int y{};
    int width{};
    int height{};

    std::optional<Layer> selectedLayer;

    Viewport viewport;
    CommandHistory viewHistory{1024};
    std::optional<ViewportChangeCommand> currentViewportCommand;

    bool dirty;
};

} // namespace Midori
