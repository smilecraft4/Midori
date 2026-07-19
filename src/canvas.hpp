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
        TileCoord tile;
        TileWriteState state;
        Renderer::TileTextureDownload rendererSlot;
    };

    struct TileReadData {
        TileCoord tile;
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

    bool LoadTile(TileCoord tile);
    bool UnloadTile(TileCoord tile);
    bool CreateTile(TileCoord tile, bool initialize = true);
    bool DestroyTile(TileCoord tile);
    bool SaveTile(TileCoord tile);
    bool DeleteTile(TileCoord tile);
    bool MergeTiles(TileCoord srcTile, TileCoord dstTile);
    bool MoveTile(TileCoord srcTile, TileCoord dstTile);
    void MarkTileDirty(TileCoord tile);

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
    std::unique_ptr<ViewportChangeCommand> currentViewportChangeCommand;

    CommandHistory canvasCommands;
    std::unique_ptr<TileModificationCommand> currentTileModificationCommand;

    slot_map<Layer, LayerInfo> layers{1024};
    std::vector<Layer> layersTree;

    hash_set<TileCoord> tilesLoaded{1024};
    hash_set<TileCoord> tilesModified{1024};
    hash_map<TileCoord, TileWriteData> tilesWriting{1024};
    hash_map<TileCoord, TileReadData> tilesReading{1024};

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
    hash_set<TileCoord, arena_allocator<TileCoord>> strokeTileAffected{strokeArena};

    std::vector<Color> canvasTexture;
    glm::ivec2 canvasTextureSize;
    std::vector<Color> DownloadCanvasTexture(glm::ivec2& size) const;
    static bool SampleTexture(const std::vector<Color>& texture, glm::ivec2 textureSize, glm::vec2 pos, Color& color);
};

} // namespace Midori
