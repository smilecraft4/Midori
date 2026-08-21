#include "canvas.hpp"

#include "app.hpp"
#include "base.hpp"
#include "colors.hpp"
#include "defines.hpp"
#include "errors.hpp"
#include "layers.hpp"
#include "renderer.hpp"
#include "states.hpp"
#include "tiles.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>

#define QOI_MALLOC(sz) Midori::Malloc(sz)
#define QOI_FREE(p)    Midori::Free(p)
#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO
#include "../deps/qoi.h"
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_assert.h>
#include <tracy/Tracy.hpp>
#include <imgui.h>
#include <string>
#include <utility>
#include <algorithm>

namespace Midori {

//
// CanvasFile
//

CanvasFile::CanvasFile(App* app) : app(app), globalArena(1_KiB), updateArena(1_KiB) {
    layerNames = reinterpret_cast<char (*)[LAYER_NAME_MAX_LEN]>(
        globalArena.allocate(LAYER_MAX * LAYER_NAME_MAX_LEN * sizeof(char)));
    std::memset(layerNames, '\0', LAYER_MAX * LAYER_NAME_MAX_LEN * sizeof(char));

    layerNames[4][4] = '&';
}

CanvasFile::~CanvasFile() = default;

Error CanvasFile::Init(size_t newId) {
    id = newId;

#ifdef NDEBUG
    const std::string path = SDL_GetPrefPath(nullptr, "midori");
#else
    const std::string path = SDL_GetPrefPath(nullptr, "midori-dev");
#endif
    stageName = std::format("{}/{}", path, id);

    return Error::None;
}

Error CanvasFile::New() {
    ZoneScoped;
    SDL_assert(!fileName.has_value() && "New() on a CanvasFile that still owns an opened fileIO; Close() first");
    SDL_assert(!fileEnv && "New() on a CanvasFile that still owns a stageIO lmdb env; Close() first");
    SDL_assert(!stageEnv && "New() on a CanvasFile that still owns a stageIO lmdb env; Close() first");

    {
        ZoneScopedN("Creating lmdb stage env");

        int rc;
        rc = mdb_env_create(&stageEnv);
        rc = mdb_env_set_maxdbs(stageEnv, CANVASFILE_DBI_COUNT);
        rc = mdb_env_set_mapsize(stageEnv, CANVASFILE_MAP_SIZE);
        rc = mdb_env_set_maxreaders(stageEnv, app->workerThreads.size() + 1);
        const auto filename = stageName.string();
        rc = mdb_env_open(stageEnv, filename.c_str(), MDB_NOSUBDIR | MDB_NOTLS | MDB_NOSYNC, CANVASFILE_FILE_MODE);

        MDB_txn* txn{};
        rc = mdb_txn_begin(stageEnv, nullptr, 0, &txn);
        rc = mdb_dbi_open(txn, CANVASFILE_DBI_TILES, MDB_INTEGERKEY | MDB_CREATE, &stageTiles);
        rc = mdb_txn_commit(txn);
    }

    {
        ZoneScopedN("Creating start canvas environment");

        const LayerCreateInfo createInfo{
            .name = "Base Layer",
            .opacity = 1.0f,
            .internal = false,
        };
        if (auto res = NewLayer(createInfo); !res.has_value()) {
            return res.error();
        }
    }

    return Error::None;
}

Error CanvasFile::Open(std::filesystem::path newFilename) {
    ZoneScoped;
    SDL_assert(!fileName.has_value() && "New() on a CanvasFile that still owns an opened fileIO; Close() first");
    SDL_assert(!stageEnv && "New() on a CanvasFile that still owns a stageIO lmdb env; Close() first");
    SDL_assert(!fileEnv && "New() on a CanvasFile that still owns a stageIO lmdb env; Close() first");
    SDL_assert(newFilename.has_filename() && "Open() needs a file path, not a directory");

    if (!std::filesystem::exists(newFilename) || !std::filesystem::is_regular_file(newFilename)) {
        return Error::FileNotFound;
    }

    int rc;
    {
        ZoneScopedN("Creating lmdb stage env");

        rc = mdb_env_create(&stageEnv);
        rc = mdb_env_set_maxdbs(stageEnv, CANVASFILE_DBI_COUNT);
        rc = mdb_env_set_mapsize(stageEnv, CANVASFILE_MAP_SIZE);
        rc = mdb_env_set_maxreaders(stageEnv, app->workerThreads.size() + 1);
        const auto filename = stageName.string();
        rc = mdb_env_open(stageEnv, filename.c_str(), MDB_NOSUBDIR | MDB_NOTLS | MDB_NOSYNC, CANVASFILE_FILE_MODE);

        MDB_txn* txn{};
        rc = mdb_txn_begin(stageEnv, nullptr, 0, &txn);
        rc = mdb_dbi_open(txn, CANVASFILE_DBI_TILES, MDB_INTEGERKEY | MDB_CREATE, &stageTiles);
        rc = mdb_txn_commit(txn);
    }

    {
        ZoneScopedN("Creating lmdb file env");

        rc = mdb_env_create(&fileEnv);
        rc = mdb_env_set_maxdbs(fileEnv, CANVASFILE_DBI_COUNT);
        rc = mdb_env_set_mapsize(fileEnv, CANVASFILE_MAP_SIZE);
        rc = mdb_env_set_maxreaders(fileEnv, app->workerThreads.size() + 1);
        const auto filename = newFilename.string();
        rc = mdb_env_open(fileEnv, filename.c_str(), MDB_NOSUBDIR | MDB_NOTLS, CANVASFILE_FILE_MODE);

        MDB_txn* txn{};
        rc = mdb_txn_begin(fileEnv, nullptr, 0, &txn);
        rc = mdb_dbi_open(txn, CANVASFILE_DBI_METADATA, MDB_CREATE, &fileMetadata);
        ;
        rc = mdb_dbi_open(txn, CANVASFILE_DBI_LAYERS, MDB_INTEGERKEY | MDB_CREATE, &fileLayers);
        rc = mdb_dbi_open(txn, CANVASFILE_DBI_TILES, MDB_INTEGERKEY | MDB_CREATE, &fileTiles);
        rc = mdb_txn_commit(txn);
    }

    {
        ZoneScopedN("Retrieving file data");

        MDB_cursor* cur{};
        MDB_val val;
        MDB_val key = {
            .mv_size = sizeof(CANVASFILE_KEY_FILE_VERSION),
            .mv_data = const_cast<char*>(CANVASFILE_KEY_FILE_VERSION),
        };

        if (!fileReadTxn) {
            rc = mdb_txn_begin(fileEnv, nullptr, MDB_RDONLY, &fileReadTxn);
        } else {
            rc = mdb_txn_renew(fileReadTxn);
        }

        // Check version
        rc = mdb_get(fileReadTxn, fileMetadata, &key, &val);
        std::memcpy(&version, val.mv_data, sizeof(Version));

        // TODO proper parsing with a fixed version data parser/writer

        // Creating the layer structure based on disk
        rc = mdb_cursor_open(fileReadTxn, fileLayers, &cur);
        rc = mdb_cursor_get(cur, &key, &val, MDB_FIRST);
        while (rc == MDB_SUCCESS) {
            SDL_assert(key.mv_size == sizeof(size_t) && "Malformed layer record");
            SDL_assert(val.mv_size >= sizeof(LayerFileEntry) && "Malformed layer record");

            LayerFileEntry entry{};
            std::memcpy(&entry, val.mv_data, sizeof(LayerFileEntry));
            SDL_assert(entry.id == *static_cast<size_t*>(key.mv_data) && "Malformed layer record");
            SDL_assert(val.mv_size != sizeof(LayerFileEntry) + entry.nameLen && "Malformed layer record");

            const LayerCreateInfo createInfo{
                .id = entry.id,
                .name = std::string_view(static_cast<const char*>(val.mv_data) + sizeof(LayerFileEntry), entry.nameLen),
                .height = entry.height,
                .opacity = entry.opacity,
                .blendMode = entry.blendMode,
                .hidden = entry.hidden,
                .mask = entry.mask,
                .locked = entry.locked,
                .internal = false,
            };
            if (auto res = NewLayer(createInfo); !res.has_value()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create layer %d while opening", id);
            }

            rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
        }

        mdb_cursor_close(cur);
        mdb_txn_reset(fileReadTxn);
    }

    fileName = std::move(newFilename);

    return Error::None;
}

Error CanvasFile::Save() {
    ZoneScoped;
    SDL_assert(fileName.has_value() && "An open lmdb env must always carry its filename");
    SDL_assert(fileEnv && "Save() needs an open lmdb env; call SaveAs() for an unsaved file");
    SDL_assert(stageEnv && "Save() needs an open lmdb env; call SaveAs() for an unsaved file");

    const WriteCommand writeData{
        .type = WriteCommand::Type::Commit,
        .canvas = id,
        .id = TileTransfer::NONE,
        .dbi = fileTiles,
    };
    {
        std::unique_lock lock(app->writeMutex);
        app->writeQueue.push_back(writeData);
    }
    app->writeCv.notify_one();

    return Error::None;
}

Error CanvasFile::SaveAs(std::filesystem::path newFilename) {
    ZoneScoped;
    SDL_assert(stageEnv);

    // TODO need to have no pending writes nor read

    // Make sure the path exists
    if (std::filesystem::exists(newFilename)) {
        if (std::filesystem::is_regular_file(newFilename)) {
            std::filesystem::remove(newFilename);
        }
    } else {
        std::filesystem::create_directories(newFilename.parent_path());
    }

    const auto filenameStr = newFilename.string();

    int rc;
    if (fileEnv) {
        // Copy the live env to the new path, then reopen there so subsequent writes
        // land in the new file. Keep the old env open if the copy fails.
        mdb_env_copy2(fileEnv, filenameStr.c_str(), MDB_CP_COMPACT);

        mdb_dbi_close(fileEnv, fileMetadata);
        mdb_dbi_close(fileEnv, fileLayers);
        mdb_dbi_close(fileEnv, fileTiles);
        mdb_env_close(fileEnv);
        fileEnv = nullptr;
    }

    rc = mdb_env_create(&fileEnv);
    rc = mdb_env_set_maxreaders(fileEnv, app->workerThreads.size() + 1);
    rc = mdb_env_set_maxdbs(fileEnv, CANVASFILE_DBI_COUNT);
    rc = mdb_env_set_mapsize(fileEnv, CANVASFILE_MAP_SIZE);
    rc = mdb_env_open(fileEnv, filenameStr.c_str(), MDB_NOSUBDIR, CANVASFILE_FILE_MODE);

    MDB_txn* txn{};
    rc = mdb_txn_begin(fileEnv, nullptr, 0, &txn);
    rc = mdb_dbi_open(txn, CANVASFILE_DBI_METADATA, MDB_CREATE, &fileMetadata);
    rc = mdb_dbi_open(txn, CANVASFILE_DBI_LAYERS, MDB_INTEGERKEY | MDB_CREATE, &fileLayers);
    rc = mdb_dbi_open(txn, CANVASFILE_DBI_TILES, MDB_INTEGERKEY | MDB_CREATE, &fileTiles);
    rc = mdb_txn_commit(txn);

    fileName = std::move(newFilename);

    return Save();
}

Error CanvasFile::Close() {
    ZoneScoped;
    SDL_assert(stageEnv && "Close() on a CanvasFile that is not New() nor Open()");

    // TODO Wait for all TileTransfers to be finished

    mdb_dbi_close(stageEnv, stageTiles);
    mdb_env_close(stageEnv);
    stageTiles = {};
    stageEnv = nullptr;
    std::filesystem::remove(stageName);

    if (fileName.has_value()) {
        mdb_dbi_close(fileEnv, fileTiles);
        mdb_dbi_close(fileEnv, fileLayers);
        mdb_dbi_close(fileEnv, fileMetadata);
        mdb_env_close(fileEnv);
        fileTiles = {};
        fileLayers = {};
        fileMetadata = {};
        fileEnv = nullptr;
    }

    return Error::None;
}

Error CanvasFile::Update() {
    ZoneScoped;

    {
        ZoneScopedN("Culling");
        for (const auto& canvasViewID : canvasViewIds) {
            if (canvasViewID.has_value()) {
                ZoneScopedN("CanvasView Culling");
                auto& canvasView = app->canvasViews.at(canvasViewID.value());

                // Filter hidden, too small, etc...

                const auto& visibleTiles = canvasView.viewport.VisibleTiles(app->frameArena);
                for (const auto& visibleTile : visibleTiles) {
                    for (const auto& layer : layersTree) {
                        const Tile tile{
                            .canvasFile = id,
                            .layer = layer,
                            .y = visibleTile.y,
                            .x = visibleTile.x,
                        };
                        if (auto* tileInfo = tileInfos.try_at(tile)) {
                            if (tileInfo->frame != app->frame) {
                                tileInfo->frame = app->frame;
                            }
                        } else {
                            LoadTile(tile);
                        }
                    }
                }
            }
        }
        {
            ZoneScopedN("Unloading Cull");
            for (const auto& [tile, tileInfo] : tileInfos) {
                if (tileInfo.frame == app->frame) {
                    continue;
                } else if (tileTransfers.contains(tile)) {
                    continue;
                } else if (((tileInfo.state & TileState::Modified) == TileState::Modified)
                           && ((tileInfo.state & TileState::Staged) != TileState::Staged)) {
                    StageTile(tile);
                } else {
                    DestroyTile(tile);
                }
            }
        }
    }

    return Error::None;
}

std::expected<Layer, Error> CanvasFile::NewLayer(LayerCreateInfo createInfo) {
    ZoneScoped;

    if (layers.full()) {
        return std::unexpected(Error::OutOfCapacity);
    }

    if (createInfo.id == std::nullopt) {
        createInfo.id = layers.free_slot();
    }

    createInfo.height = std::min(createInfo.height, static_cast<LayerHeight>(layersTree.size()));

    LayerInfo info{
        .id = createInfo.id.value(),
        .height = createInfo.height,
        .opacity = createInfo.opacity,
        .blendMode = createInfo.blendMode,
        .hidden = createInfo.hidden,
        .mask = createInfo.mask,
        .locked = createInfo.locked,
        .internal = createInfo.internal,
        .modified = true,
    };

    if (!layers.emplace_at(createInfo.id.value(), std::move(info))) {
        return std::unexpected(Error::OutOfCapacity);
    }
    std::memset(layerNames[createInfo.id.value()], 0, sizeof(layerNames[createInfo.id.value()]));
    std::memcpy(layerNames[createInfo.id.value()], createInfo.name.data(),
                std::min(createInfo.name.size(), sizeof(layerNames[createInfo.id.value()]) - 1));

    for (const auto& id : canvasViewIds) {
        if (!id.has_value()) {
            continue;
        }

        if (!app->renderer.CreateLayerTexture(app->canvasViews.at(id.value()), createInfo.id.value())) {
            layers.erase(createInfo.id.value());
            return std::unexpected(Error::Unknown);
        }
    }

    layersTree.push_back(createInfo.id.value());
    layers.at(createInfo.id.value()).height = static_cast<LayerHeight>(layersTree.size() - 1);
    SetLayerHeight(createInfo.id.value(), createInfo.height);

    layers.at(createInfo.id.value()).tiles.reserve(TILES_MAX);

    return createInfo.id.value();
}

std::expected<Layer, Error> CanvasFile::GetLayer(LayerHeight height) {
    ZoneScoped;
    if (height > layersTree.size()) {
        return std::unexpected(Error::LayerNotFound);
    }

    return layersTree[height];
}

Error CanvasFile::DeleteLayer(Layer layer) {
    ZoneScoped;

    if (!layers.contains(layer)) {
        return Error::LayerNotFound;
    }

    std::vector<Tile, arena_allocator<Tile>> tilesDeleting(app->frameArena);
    for (const Tile& tile : layers.at(layer).tiles) {
        tilesDeleting.push_back(tile);
    }

    for (const auto& tile : tilesDeleting) {
        DestroyTile(tile);
        DeleteTile(tile);
    }

    SetLayerHeight(layer, static_cast<LayerHeight>(layersTree.size() - 1));
    layersTree.pop_back();

    for (const auto& id : canvasViewIds) {
        if (!id.has_value()) {
            continue;
        }

        app->renderer.DestroyLayerTexture(app->canvasViews.at(id.value()), layer);
    }
    layers.erase(layer);

    return Error::None;
}

Error CanvasFile::SetLayerName(Layer layer, std::string newName) {
    ZoneScoped;

    if (!layers.contains(layer)) {
        return Error::LayerNotFound;
    }

    LayerInfo& info = layers.at(layer);
    info.name = std::move(newName);
    info.modified = true;

    return Error::None;
}

Error CanvasFile::SetLayerHeight(Layer layer, LayerHeight newHeight) {
    ZoneScoped;

    if (!layers.contains(layer)) {
        return Error::LayerNotFound;
    }

    if (layersTree.size() <= 1) {
        return Error::None;
    }

    const size_t oldHeight = layers.at(layer).height;
    const size_t target = std::min<size_t>(newHeight, layersTree.size() - 1);
    if (target == oldHeight) {
        return Error::None;
    }

    const auto begin = layersTree.begin();
    if (target > oldHeight) {
        std::rotate(begin + static_cast<std::ptrdiff_t>(oldHeight), begin + static_cast<std::ptrdiff_t>(oldHeight) + 1,
                    begin + static_cast<std::ptrdiff_t>(target) + 1);
    } else {
        std::rotate(begin + static_cast<std::ptrdiff_t>(target), begin + static_cast<std::ptrdiff_t>(oldHeight),
                    begin + static_cast<std::ptrdiff_t>(oldHeight) + 1);
    }

    for (size_t i = std::min(oldHeight, target); i <= std::max(oldHeight, target); i++) {
        LayerInfo& info = layers.at(layersTree[i]);
        info.height = static_cast<LayerHeight>(i);
        info.modified = true;
    }

    return Error::None;
}

Error CanvasFile::SetLayerBlendMode(Layer layer, BlendMode newBlendMode) {
    ZoneScoped;

    if (!layers.contains(layer)) {
        return Error::LayerNotFound;
    }

    LayerInfo& info = layers.at(layer);
    info.blendMode = newBlendMode;
    info.modified = true;

    return Error::None;
}

Error CanvasFile::MergeLayers(Layer srcLayer, Layer dstLayer) {
    ZoneScoped;
    SDL_assert(layers.contains(srcLayer) && "Merge source layer does not exist");
    SDL_assert(layers.contains(dstLayer) && "Merge destination layer does not exist");

    size_t failed{};
    Error err;
    for (const auto& srcTile : layers.at(srcLayer).tiles) {
        SDL_assert(tileStates.contains(srcTile));
        SDL_assert(srcTile.layer == srcLayer);

        const Tile dstTile{
            .layer = dstLayer,
            .y = srcTile.y,
            .x = srcTile.x,
        };

        if (!IsTileLoaded(tileStates.at(srcTile))) {
            err = LoadTile(srcTile);
            if (err != Error::None) {
                failed++;
                continue;
            }
        }

        if (!tileStates.contains(dstTile)) {
            err = NewTile(dstTile);
            if (err != Error::None) {
                failed++;
                continue;
            }
        } else if (!IsTileLoaded(tileStates.at(dstTile))) {
            err = LoadTile(dstTile);
            if (err != Error::None) {
                failed++;
                continue;
            }
        }

        err = MergeTiles(srcTile, dstTile);
        if (err != Error::None) {
            failed++;
            continue;
        }
    }

    // Found a way to delete this layer once the transfer are done
    // How to recover when a fail occur
    // Converting most complex layer functions (merge, duplicate, delete) may need a command of there own
    // This would allow starting a merge, Update and only when the merge is finished or failed exit the command state
    // The next big concern is how to handle undo/redo, all thoses tile may (in worst case) be 2GiB of data. Which could
    // seat on RAM on my machine. But instead caching the version to disk may be better
    // Another mechanism such as ZRAM could also be used where multiple tileTextures get's compressed on the gpu (diet
    // gpu) directly. Or download them then pack them and compress them together.
    // Another possibility is to store the unloaded tile history to the stageEnv in a versionDBI where the tile get's an
    // added tag for history and when loading the tile we load the tile history as well
    // Another cheap trick would be to warn the user that this merge is too large to be undoable/redoable

    return failed == 0 ? Error::None : Error::Unknown;
}

std::expected<Layer, Error> CanvasFile::DuplicateLayer(Layer layer, std::optional<bool> internal) {
    ZoneScoped;
    SDL_assert(layers.contains(layer) && "Duplicating layer does not exists");

    Error err;
    size_t failed{};
    const auto& layerInfo = layers.at(layer);

    const LayerCreateInfo createInfo{
        .name = layerInfo.name,
        .height = static_cast<LayerHeight>(layerInfo.height + 1),
        .opacity = layerInfo.opacity,
        .blendMode = layerInfo.blendMode,
        .hidden = layerInfo.hidden,
        .mask = layerInfo.mask,
        .locked = layerInfo.locked,
        .internal = internal.value_or(createInfo.internal),
    };
    const auto dstLayer = NewLayer(createInfo);
    if (!dstLayer.has_value()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create new layer");
        return std::unexpected(dstLayer.error());
    }

    for (const auto& srcTile : layerInfo.tiles) {
        if (!IsTileLoaded(tileStates.at(srcTile))) {
            err = LoadTile(srcTile);
            if (err != Error::None) {
                failed++;
                continue;
            }
        }

        const Tile dstTile{
            .layer = dstLayer.value(),
            .y = srcTile.y,
            .x = srcTile.x,
        };

        err = DuplicateTile(srcTile, dstTile);
        if (err != Error::None) {
            failed++;
            continue;
        }
    }

    return dstLayer.value();
}

Error CanvasFile::NewTile(Tile tile) {
    ZoneScoped;
    // TODO
    // Do the necesserary tileState checks and issue the tileCommands for i
    SDL_assert(tile.canvas == id && "NewTile() on a tile that belongs to another canvas file");
    SDL_assert(layers.contains(tile.layer) && "NewTile() on a layer that does not exist");
    SDL_assert(!tileStates.contains(tile) && "NewTile() on a tile that already exists");
    SDL_assert(!tileTransfers.contains(tile));

    const TileCommand tileCommand{
        .type = TileCommand::Type::New,
        .seq = app->tileCommandCounter++,
        .srcTile = tile,
    };
    {
        std::unique_lock lock(app->tileCommandsMutex);
        app->tileCommandsQueue.push_back(tileCommand);
    }

    const TileCommand command{
        .type = TileCommand::Type::Render,
        .seq = app->tileCommandCounter++,
        .srcTile = tile,
    };
    app->pendingTileCommands.push_back(command);

    return Error::None;
}

Error CanvasFile::LoadTile(Tile tile) {
    ZoneScoped;
    // TODO
    // Do the necesserary tileState checks and issue the tileCommands for i
    SDL_assert(tile.canvas == this->id && "LoadTile() on a tile that belongs to another canvas file");
    SDL_assert(layers.contains(tile.layer) && "LoadTile() on a layer that does not exist");
    SDL_assert(tileStates.contains(tile) && "LoadTile() on a tile with no state entry");
    SDL_assert(!IsTileLoaded(tileStates.at(tile)) && "LoadTile() on a tile that is already loaded");
    SDL_assert(!IsTileDeleted(tileStates.at(tile)) && "LoadTile() on a tile that is staged as deleted");

    if (tileTransfers.contains(tile)) {
        auto id = tileTransfers.at(tile);
        if (app->tileTransfers.at(id).read != TileReadState::None) {
            return Error::None; // The tile is already being loaded
        }
    }

    const TileCommand tileCommand{
        .type = TileCommand::Type::Load,
        .seq = app->tileCommandCounter++,
        .srcTile = tile,
    };
    {
        std::unique_lock lock(app->tileCommandsMutex);
        app->tileCommandsQueue.push_back(tileCommand);
    }

    return Error::None;
}

Error CanvasFile::DestroyTile(Tile tile) {
    ZoneScoped;
    // TODO
    // Do the necesserary tileState checks and issue the tileCommands for i
    SDL_assert(tile.canvas == id && "DestroyTile() on a tile that belongs to another canvas file");
    // SDL_assert(tileStates.contains(tile) && "DestroyTile() on a tile with no state entry");
    // SDL_assert(!IsTileTransfering(tileStates.at(tile)) && "DestroyTile() on a tile that is still transfering");
    // SDL_assert(!tileTransfers.contains(tile) && "DestroyTile() on a tile that has a transfer in flight");
    // SDL_assert((!IsTileModified(tileStates.at(tile)) || IsTileStaged(tileStates.at(tile)))
    //            && "DestroyTile() would drop unstaged modifications");

    const TileCommand tileCommand{
        .type = TileCommand::Type::Destroy,
        .seq = app->tileCommandCounter++,
        .srcTile = tile,
    };
    app->tileCommandsQueue.push_back(tileCommand);
    return Error::None;
}

Error CanvasFile::DeleteTile(Tile tile) {
    ZoneScoped;
    // TODO
    // Do the necesserary tileState checks and issue the tileCommands for i
    SDL_assert(tile.canvas == id && "DeleteTile() on a tile that belongs to another canvas file");
    // SDL_assert(tileStates.contains(tile) && "DeleteTile() on a tile with no state entry");
    // SDL_assert(!IsTileDeleted(tileStates.at(tile)) && "DeleteTile() on a tile that is already staged as deleted");
    // SDL_assert(!IsTileTransfering(tileStates.at(tile)) && "DeleteTile() on a tile that is still transfering");

    const TileCommand tileCommand{
        .type = TileCommand::Type::Delete,
        .seq = app->tileCommandCounter++,
        .srcTile = tile,
    };
    app->tileCommandsQueue.push_back(tileCommand);

    return Error::None;
}

Error CanvasFile::UnloadTile(Tile tile) {
    ZoneScoped;
    // TODO
    // Do the necesserary tileState checks and issue the tileCommands for i
    SDL_assert(tile.canvas == id && "UnloadTile() on a tile that belongs to another canvas file");
    SDL_assert(tileStates.contains(tile));

    const auto& tileState = tileStates.at(tile);
    if (!IsTileLoaded(tileState)) {
        return Error::None;
    }

    SDL_assert(IsTileLoaded(tileState));
    SDL_assert(!IsTileDeleted(tileState) && "UnloadTile() on a tile that is staged as deleted");

    if (IsTileModified(tileState) && !IsTileStaged(tileState) && !IsTileTransfering(tileState)) {
        StageTile(tile);
    } else if (!IsTileTransfering(tileState)) {
        DestroyTile(tile);
    }

    return Error::None;
}

Error CanvasFile::MergeTiles(Tile srcTile, Tile dstTile) {
    ZoneScoped;
    // TODO
    // Do the necesserary tileState checks and issue the tileCommands for i
    SDL_assert(srcTile.canvas == id && dstTile.canvas == id && "MergeTiles() across canvas files");
    SDL_assert(!(srcTile == dstTile) && "MergeTiles() onto itself");
    // SDL_assert(tileStates.contains(srcTile));
    // SDL_assert(tileStates.contains(dstTile));
    SDL_assert(layers.contains(srcTile.layer));
    SDL_assert(layers.contains(dstTile.layer));

    // auto& srcTileState = tileStates.at(srcTile);
    // SDL_assert(IsTileLoaded(srcTileState));
    // SDL_assert(!IsTileTransfering(srcTileState) && "MergeTiles() source is still transfering");

    // auto& dstTileState = tileStates.at(dstTile);
    // SDL_assert(IsTileLoaded(dstTileState));
    // SDL_assert(!IsTileTransfering(dstTileState) && "MergeTiles() destination is still transfering");
    // SDL_assert(false && "MergeTiles() never records a Merge command; it only destroys the source");

    // How to make sure the layer does not get destroyed while doing a merge that needs a tile load ?

    const auto& srcLayerInfo = layers.at(srcTile.layer);
    const TileCommand tileCommand{
        .type = TileCommand::Type::Merge,
        .seq = app->tileCommandCounter++,
        .srcTile = srcTile,
        .dstTile = dstTile,
        .blendMode = srcLayerInfo.blendMode,
        .opacity = srcLayerInfo.opacity,
    };
    {
        std::unique_lock lock(app->tileCommandsMutex);
        app->tileCommandsQueue.push_back(tileCommand);
    }

    tileStates.at(srcTile) &= ~TileState::Modified;
    if (const auto res = DestroyTile(srcTile); res != Error::None) {
        return res;
    }

    if (const auto res = DeleteTile(srcTile); res != Error::None) {
        return res;
    }

    return Error::None;
}

Error CanvasFile::DuplicateTile(Tile srcTile, Tile dstTile) {
    ZoneScoped;
    // TODO
    // Do the necesserary tileState checks and issue the tileCommands for it
    SDL_assert(srcTile.canvas == id && dstTile.canvas == id && "DuplicateTile() across canvas files");
    SDL_assert(!(srcTile == dstTile) && "DuplicateTile() onto itself");
    SDL_assert(layers.contains(srcTile.layer) && "DuplicateTile() source layer does not exist");
    SDL_assert(layers.contains(dstTile.layer) && "DuplicateTile() destination layer does not exist");
    SDL_assert(tileStates.contains(srcTile) && "DuplicateTile() source has no state entry");
    SDL_assert(IsTileLoaded(tileStates.at(srcTile)) && "DuplicateTile() source is not loaded");
    SDL_assert(!tileStates.contains(dstTile) && "DuplicateTile() destination already exists");

    const TileCommand tileCommand{
        .type = TileCommand::Type::Duplicate,
        .seq = app->tileCommandCounter++,
        .srcTile = srcTile,
        .dstTile = dstTile,
    };
    {
        std::unique_lock lock(app->tileCommandsMutex);
        app->tileCommandsQueue.push_back(tileCommand);
    }

    return Error::None;
}

Error CanvasFile::StageTile(Tile tile) {
    ZoneScoped;
    // TODO
    // Do the necesserary tileState checks and issue the tileCommands for i
    SDL_assert(tile.canvas == id && "StageTile() on a tile that belongs to another canvas file");
    SDL_assert(tileStates.contains(tile));
    SDL_assert(!tileTransfers.contains(tile) && "StageTile() on a tile that has a transfer in flight");

    const auto tileState = tileStates.at(tile);
    SDL_assert(IsTileLoaded(tileState));
    SDL_assert(IsTileModified(tileState));
    SDL_assert(!IsTileDeleted(tileState));
    SDL_assert(!IsTileTransfering(tileState) && "StageTile() on a tile that is still transfering");

    const TileCommand tileCommand{
        .type = TileCommand::Type::Stage,
        .seq = app->tileCommandCounter++,
        .srcTile = tile,
    };
    {
        std::unique_lock lock(app->tileCommandsMutex);
        app->tileCommandsQueue.push_back(tileCommand);
    }
    return Error::None;
}

//
// CanvasView
//

CanvasView::CanvasView(App* app, size_t windowId, size_t canvasFileId)
    : app(app), windowId(windowId), canvasFileId(canvasFileId) {
    SDL_assert(app->canvasFiles.contains(canvasFileId) && "CanvasView references a canvas file that no longer exists");

    auto& canvasFile = app->canvasFiles.at(canvasFileId);
}

CanvasView::~CanvasView() = default;

Error CanvasView::Init(size_t newId) {
    ZoneScoped;
    auto& canvasFile = app->canvasFiles.at(canvasFileId);

    id = newId;
    width = app->windowSize.x;
    height = app->windowSize.y;

    viewport.SetRotation(0.0f);
    viewport.SetTranslation(glm::vec2{0.0f});
    viewport.SetZoom(glm::vec2{1.0f});
    viewport.ComputeViewMatrix();

    bool found{false};
    for (auto& canvasViewId : canvasFile.canvasViewIds) {
        if (!canvasViewId.has_value()) {
            canvasViewId = newId;
            found = true;
            break;
        }
    }
    if (!found) {
        return Error::OutOfCapacity;
    }

    for (const auto& [id, info] : canvasFile.layers) {
        app->renderer.CreateLayerTexture(*this, id);
    }

    return Error::None;
}

Error CanvasView::Update() {
    ZoneScoped;
    auto& canvasFile = app->canvasFiles.at(canvasFileId);

    if (dirty) {
        dirty = false;
    }

    {
        ZoneScopedN("Culling Load");
        const auto& visibleTiles = viewport.VisibleTiles(app->frameArena);
        for (const auto& visibleTile : visibleTiles) {
            for (const auto& [id, info] : canvasFile.layers) {
                const Tile tile{
                    .canvasFile = canvasFile.id,
                    .layer = info.id,
                    .y = visibleTile.y,
                    .x = visibleTile.x,
                };
                if (auto* tileInfo = canvasFile.tileInfos.try_at(tile)) {
                    if (tileInfo->frame != app->frame) {
                        tileInfo->frame = app->frame;
                    }
                } else {
                    canvasFile.LoadTile(tile);
                }
            }
        }
    }

    return Error::None;
}

Error CanvasView::Quit() {
    ZoneScoped;
    auto& canvasFile = app->canvasFiles.at(canvasFileId);

    const auto& visibleTiles = viewport.VisibleTiles(app->frameArena);
    for (const auto& [tile, state] : canvasFile.tileStates) {
        if (canvasFile.layers.at(tile.layer).hidden || !viewport.IsTileVisible({tile.x, tile.y})) {
            canvasFile.UnloadTile(tile);
        }
    }

    for (const auto& [id, info] : canvasFile.layers) {
        app->renderer.DestroyLayerTexture(*this, id);
    }

    for (auto& canvasViewId : canvasFile.canvasViewIds) {
        if (id == canvasViewId) {
            canvasViewId = std::nullopt;
        }
    }

    return Error::None;
}

Error CanvasView::Resize(int newWidth, int newHeight) {
    ZoneScoped;
    width = newWidth;
    height = newHeight;
    viewport.Resize(glm::vec2{width, height});
    app->renderer.ResizeCanvasView(*this);
    return Error::None;
}

Error CanvasView::Move(int newX, int newY) {
    ZoneScoped;
    x = newX;
    y = newY;
    return Error::None;
}

Error CanvasView::SelectLayer(Layer layer) {
    ZoneScoped;
    selectedLayer = layer;
    return Error::None;
}

Error CanvasView::DeselectedLayer(Layer layer) {
    ZoneScoped;
    if (selectedLayer == layer) {
        selectedLayer = std::nullopt;
    }
    return Error::None;
}
} // namespace Midori
