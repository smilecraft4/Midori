#pragma once

#include "brushes.hpp"
#include "colors.hpp"
#include "commands.hpp"
#include "defines.hpp"
#include "layers.hpp"
#include "renderer.hpp"
#include "states.hpp"
#include "tiles.hpp"
#include "viewport.hpp"
#include "base.hpp"

#include <SDL3/SDL_gpu.h>
#include <array>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <cstdint>

namespace Midori {

struct App;

struct Canvas {
    enum class Error : uint8_t {
        None,
        LayerNotFound,
        TileNotFound,
        Unknown,
    };

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

    struct StrokePoint {
        glm::vec4 color = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        glm::vec2 position;
        float radius = 8.0f;
        float flow = 0.5f;
        float hardness = 0.5f;
        float pad[3];
    };

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

    BrushSettings paintBrush;
    bool paintBrushModified{};
    BrushSettings eraseBrush;
    bool eraseBrushModified{};

    StrokePoint previousPoint{};
    std::vector<StrokePoint, arena_allocator<StrokePoint>> strokePoints{strokeArena};
    hash_set<Tile, arena_allocator<Tile>> strokeTileAffected{strokeArena};

    std::vector<Color> canvasTexture;
    glm::ivec2 canvasTextureSize;
    std::vector<Color> DownloadCanvasTexture(glm::ivec2& size) const;
    static bool SampleTexture(const std::vector<Color>& texture, glm::ivec2 textureSize, glm::vec2 pos, Color& color);
};

struct CanvasFile;
struct CanvasView;
struct Window;

struct CanvasFile {
    CanvasFile(const CanvasFile&) = delete;
    CanvasFile(CanvasFile&&) = delete;
    CanvasFile& operator=(const CanvasFile&) = delete;
    CanvasFile& operator=(CanvasFile&&) = delete;

    CanvasFile(App* app);
    ~CanvasFile() = default;

    void Init();
    void Update();
    void Quit();

    Layer NewLayer(LayerCreateInfo createInfo);
    bool DeleteLayer(Layer layer);
    bool RenameLayer(Layer layer, std::string newName);
    bool MoveLayer(Layer layer, LayerHeight newHeight);

    bool NewTile(Tile tile);
    bool DeleteTile(Tile tile);
    bool ReadTile(Tile tile);
    bool WriteTile(Tile tile);

    App* app;
    std::array<CanvasView*, 16> canvasViews;
};

struct CanvasView {
    CanvasView(const CanvasView&) = delete;
    CanvasView(CanvasView&&) = delete;
    CanvasView& operator=(const CanvasView&) = delete;
    CanvasView& operator=(CanvasView&&) = delete;

    CanvasView(App* app);
    ~CanvasView() = default;

    void Init();
    void Update();
    void Render();
    void Quit();

    bool HideLayer(Layer layer, bool hide);
    bool LockLayer(Layer layer, bool lock);
    bool SelectLayer(Layer layer);

    bool LoadTile();
    bool UnloadTile();

    App* app;
    Window* window;
    CanvasFile* canvasFile;
};

} // namespace Midori
