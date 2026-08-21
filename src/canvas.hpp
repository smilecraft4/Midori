#pragma once

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
#include <cstdint>
#include <cstring>
#include <expected>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <filesystem>
#include <optional>

namespace Midori {

struct App;

struct CanvasFile {
    CanvasFile(const CanvasFile&) = delete;
    CanvasFile(CanvasFile&&) noexcept = delete;
    CanvasFile& operator=(const CanvasFile&) = delete;
    CanvasFile& operator=(CanvasFile&&) noexcept = delete;

    explicit CanvasFile(App* app);
    ~CanvasFile();

    Error Init(size_t newId);

    /// Create a new document from scratch
    /// The file has no filename so it cannot be Save()
    /// Only SaveAs(newName) will work
    Error New();

    /// Save a document under a new name
    Error SaveAs(std::filesystem::path newFilename);

    /// Open an existing document at the path
    Error Open(std::filesystem::path newFilename);

    /// Save a file (any staged/unstage changes, layers, etc...)
    /// Must have an existing filename to work
    Error Save();

    /// Close a file and free all it related data (destroys the stageEnv file, any related renderer textures etc...)
    Error Close();

    // Update the transfers ???
    Error Update();

    std::expected<Layer, Error> NewLayer(LayerCreateInfo createInfo);
    std::expected<Layer, Error> GetLayer(LayerHeight height);
    Error DeleteLayer(Layer layer);
    Error SetLayerName(Layer layer, std::string newName);
    Error SetLayerHeight(Layer layer, LayerHeight newHeight);
    Error SetLayerBlendMode(Layer layer, BlendMode newBlendMode);
    Error MergeLayers(Layer srcLayer, Layer dstLayer);
    std::expected<Layer, Error> DuplicateLayer(Layer layer, std::optional<bool> internal = std::nullopt);

    // Is somewhere
    bool HasTile(Tile tile) const;

    // Is in the session vram and ram
    bool IsTileResident(Tile tile) const;

    // Is in the session disk cache
    bool IsTileStaged(Tile tile) const;

    // Is in the the file
    bool IsTileStored(Tile tile) const;

    Error LoadTile(Tile tile);
    Error NewTile(Tile tile);
    Error MergeTiles(Tile src, Tile dst);
    Error DuplicateTile(Tile src, Tile dst);
    Error DestroyTile(Tile tile);
    Error DeleteTile(Tile tile);
    Error UnloadTile(Tile tile);
    Error StageTile(Tile tile);

    App* app;
    CanvasFileID id;
    Version version{CANVASFILE_VERSION};

    arena<> globalArena{1_MiB};
    arena<> updateArena;

    std::array<std::optional<size_t>, CANVASVIEW_MAX> canvasViewIds{};

    std::filesystem::path stageName;
    MDB_env* stageEnv{};
    MDB_dbi stageTiles{};
    MDB_txn* stageReadTxn{};

    std::optional<std::filesystem::path> fileName;
    MDB_env* fileEnv{};
    MDB_dbi fileMetadata{};
    MDB_dbi fileLayers{};
    MDB_dbi fileTiles{};
    MDB_txn* fileReadTxn{};

    std::vector<Layer> layersTree;
    slot_map<Layer, LayerInfo> layers{1_KiB};
    char (*layerNames)[LAYER_NAME_MAX_LEN];

    hash_map<Tile, TileInfo> tileInfos{2_KiB};
    hash_map<Tile, TileTransfer::ID> tileTransfers{1_KiB};

    CommandHistory canvasHistory{256};
    std::optional<TileModificationCommand> currentTileCommand;
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

    App* app;
    size_t id;
    size_t windowId;
    size_t canvasFileId;
    int x{};
    int y{};
    int width{};
    int height{};

    std::optional<Layer> selectedLayer;
    std::vector<Tile> tileVisibles;

    Viewport viewport;
    CommandHistory viewHistory{512};
    std::optional<ViewportChangeCommand> currentViewportCommand;

    bool dirty;
};

} // namespace Midori
