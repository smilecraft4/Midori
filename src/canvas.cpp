#include "canvas.hpp"

#include "SDL3/SDL_gpu.h"
#include "SDL3/SDL_log.h"
#include "app.hpp"
#include "base.hpp"
#include "brushes.hpp"
#include "colors.hpp"
#include "defines.hpp"
#include "errors.hpp"
#include "layers.hpp"
#include "renderer.hpp"
#include "states.hpp"
#include "tiles.hpp"
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <optional>

// qoi.h is implemented at global scope, so these have to be qualified.
#define QOI_MALLOC(sz) Midori::Malloc(sz)
#define QOI_FREE(p)    Midori::Free(p)

#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO
#include "../deps/qoi.h"
#include "../deps/json.hpp"
#include <SDL3/SDL_assert.h>
#include <tracy/Tracy.hpp>
#include <imgui.h>
#include <string>
#include <utility>
#include <algorithm>
#include <format>

namespace Midori {

//
// CanvasFile
//

CanvasFile::CanvasFile(App* app) : app(app) {
    ZoneScoped;
}

CanvasFile::~CanvasFile() = default;

Error CanvasFile::New() {
    ZoneScoped;
    SDL_assert(!env);
    SDL_assert(!filename.has_value());

    const LayerCreateInfo createInfo{
        .name = "Base Layer",
        .opacity = 1.0f,
        .internal = false,
    };
    if (auto res = NewLayer(createInfo); !res.has_value()) {
        return res.error();
    }

    SyncCanvasViews();

    return Error::None;
}

Error CanvasFile::Open(std::filesystem::path newFilename) {
    ZoneScoped;
    SDL_assert(!env);
    SDL_assert(newFilename.has_filename());

    if (!std::filesystem::exists(newFilename) || !std::filesystem::is_regular_file(newFilename)) {
        return Error::FileNotFound;
    }

    const auto filenameStr = newFilename.string();

    int rc = mdb_env_create(&env);
    if (rc == MDB_SUCCESS) {
        rc = mdb_env_set_maxdbs(env, CANVASFILE_DBI_COUNT);
    }
    if (rc == MDB_SUCCESS) {
        rc = mdb_env_set_mapsize(env, CANVASFILE_MAP_SIZE);
    }
    if (rc == MDB_SUCCESS) {
        rc = mdb_env_open(env, filenameStr.c_str(), MDB_NOSUBDIR, CANVASFILE_FILE_MODE);
    }
    if (rc != MDB_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open lmdb env '%s': %s", filenameStr.c_str(),
                     mdb_strerror(rc));
        SDL_assert(false && "Failed to open the lmdb environment");
        if (env) {
            mdb_env_close(env);
            env = nullptr;
        }
        return Error::Unknown;
    }

    MDB_txn* txn{};
    MDB_val key{};
    MDB_val val{};

    rc = mdb_txn_begin(env, nullptr, 0, &txn);
    if (rc != MDB_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to begin the dbi open transaction: %s", mdb_strerror(rc));
        SDL_assert(false && "Failed to begin the lmdb dbi open transaction");
        mdb_env_close(env);
        env = nullptr;
        return Error::Unknown;
    }

    rc = mdb_dbi_open(txn, CANVASFILE_DBI_METADATA, MDB_CREATE, &dbiMetadata);
    if (rc == MDB_SUCCESS) {
        rc = mdb_dbi_open(txn, CANVASFILE_DBI_LAYERS, MDB_INTEGERKEY | MDB_CREATE, &dbiLayers);
    }
    if (rc == MDB_SUCCESS) {
        rc = mdb_dbi_open(txn, CANVASFILE_DBI_TILES, MDB_INTEGERKEY | MDB_CREATE, &dbiTiles);
    }
    if (rc == MDB_SUCCESS) {
        rc = mdb_txn_commit(txn);
    } else {
        mdb_txn_abort(txn);
    }
    if (rc != MDB_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open the lmdb databases: %s", mdb_strerror(rc));
        SDL_assert(false && "Failed to open the lmdb databases");
        mdb_env_close(env);
        env = nullptr;
        return Error::Unknown;
    }

    // Read the file version
    rc = mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn);
    if (rc != MDB_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to begin the file read transaction: %s", mdb_strerror(rc));
        SDL_assert(false && "Failed to begin the lmdb file read transaction");
        mdb_env_close(env);
        env = nullptr;
        return Error::Unknown;
    }

    key.mv_data = const_cast<char*>(CANVASFILE_KEY_FILE_VERSION);
    key.mv_size = sizeof(CANVASFILE_KEY_FILE_VERSION);
    rc = mdb_get(txn, dbiMetadata, &key, &val);
    if (rc != MDB_SUCCESS || val.mv_size != sizeof(Version)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read the file version (%zu bytes): %s", val.mv_size,
                     mdb_strerror(rc));
        SDL_assert(false && "File version record missing or the wrong size");
        mdb_txn_abort(txn);
        mdb_env_close(env);
        env = nullptr;
        return Error::Unknown;
    }
    std::memcpy(&version, val.mv_data, sizeof(Version));

    MDB_cursor* cur{};
    rc = mdb_cursor_open(txn, dbiLayers, &cur);
    if (rc != MDB_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open the layer cursor: %s", mdb_strerror(rc));
        SDL_assert(false && "Failed to open the lmdb layer cursor");
        mdb_txn_abort(txn);
        mdb_env_close(env);
        env = nullptr;
        return Error::Unknown;
    }

    // A malformed record only costs one layer, so log it and keep reading. These
    // cannot be asserts: the sizes below guard memcpy against a corrupt file, and
    // asserts are compiled out of release builds.
    rc = mdb_cursor_get(cur, &key, &val, MDB_FIRST);
    while (rc == MDB_SUCCESS) {
        if (key.mv_size != sizeof(size_t) || val.mv_size < sizeof(LayerFileEntry)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Skipping layer record: %zu byte key, %zu byte value",
                         key.mv_size, val.mv_size);
        } else {
            size_t id{};
            std::memcpy(&id, key.mv_data, sizeof(id));

            LayerFileEntry entry{};
            std::memcpy(&entry, val.mv_data, sizeof(LayerFileEntry));

            if (entry.id != id) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Skipping layer %zu: record claims id %u", id, entry.id);
            } else if (val.mv_size != sizeof(LayerFileEntry) + entry.nameLen) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Skipping layer %zu: %zu byte value, expected %zu", id,
                             val.mv_size, sizeof(LayerFileEntry) + entry.nameLen);
            } else {
                const LayerCreateInfo createInfo{
                    .id = entry.id,
                    .name = std::string(static_cast<const char*>(val.mv_data) + sizeof(LayerFileEntry), entry.nameLen),
                    .height = entry.height,
                    .opacity = entry.opacity,
                    .blendMode = entry.blendMode,
                    .hidden = entry.hidden,
                    .mask = entry.mask,
                    .locked = entry.locked,
                    .internal = false,
                };
                if (auto res = NewLayer(createInfo); !res.has_value()) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create layer %zu while opening", id);
                }
            }
        }

        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
    }
    mdb_cursor_close(cur);
    cur = nullptr;

    // MDB_NOTFOUND is how a cursor reports the end of the database.
    if (rc != MDB_NOTFOUND) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed while iterating layers: %s", mdb_strerror(rc));
        SDL_assert(false && "Failed while iterating the lmdb layer database");
        mdb_txn_abort(txn);
        mdb_env_close(env);
        env = nullptr;
        return Error::Unknown;
    }

    rc = mdb_cursor_open(txn, dbiTiles, &cur);
    if (rc != MDB_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open the tile cursor: %s", mdb_strerror(rc));
        SDL_assert(false && "Failed to open the lmdb tile cursor");
        mdb_txn_abort(txn);
        mdb_env_close(env);
        env = nullptr;
        return Error::Unknown;
    }

    rc = mdb_cursor_get(cur, &key, &val, MDB_FIRST);
    while (rc == MDB_SUCCESS) {
        if (key.mv_size != sizeof(Tile)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Skipping tile record: %zu byte key, expected %zu", key.mv_size,
                         sizeof(Tile));
        } else {
            Tile tile{};
            std::memcpy(&tile, key.mv_data, sizeof(tile));

            if (layers.contains(tile.layer)) {
                LayerInfo& info = layers.at(tile.layer);
                if (info.tiles.full()) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Layer %u is at its %zu tile limit", tile.layer,
                                 info.tiles.capacity());
                } else {
                    info.tiles.insert(tile);
                }
            }
        }

        rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
    }
    mdb_cursor_close(cur);
    cur = nullptr;

    if (rc != MDB_NOTFOUND) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed while iterating tiles: %s", mdb_strerror(rc));
        SDL_assert(false && "Failed while iterating the lmdb tile database");
        mdb_txn_abort(txn);
        mdb_env_close(env);
        env = nullptr;
        return Error::Unknown;
    }

    // Read-only transactions have nothing to commit.
    mdb_txn_abort(txn);

    filename = std::move(newFilename);

    SyncCanvasViews();

    return Error::None;
}

Error CanvasFile::Save() {
    ZoneScoped;
    SDL_assert(env && "Save() needs an open lmdb env; call SaveAs() for an unsaved file");
    SDL_assert(filename.has_value() && "An open lmdb env must always carry its filename");
    if (!env) {
        return Error::FileNotFound;
    }

    for (const auto& tileModified : tilesModified) {
        WriteTile(tileModified);
    }

    MDB_txn* txn{};
    MDB_val key{};
    MDB_val val{};

    int rc = mdb_txn_begin(env, nullptr, 0, &txn);
    if (rc != MDB_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to begin the save transaction: %s", mdb_strerror(rc));
        SDL_assert(false && "Failed to begin the lmdb save transaction");
        return Error::Unknown;
    }

    key.mv_data = const_cast<char*>(CANVASFILE_KEY_FILE_VERSION);
    key.mv_size = sizeof(CANVASFILE_KEY_FILE_VERSION);
    val.mv_data = &version;
    val.mv_size = sizeof(version);
    rc = mdb_put(txn, dbiMetadata, &key, &val, 0);
    if (rc != MDB_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write the file version: %s", mdb_strerror(rc));
        SDL_assert(false && "Failed to write the file version");
        mdb_txn_abort(txn);
        return Error::Unknown;
    }

    for (const auto& layer : layers) {
        if (!layer.second.modified) {
            continue;
        }

        size_t id = layer.first;
        key.mv_data = &id;
        key.mv_size = sizeof(id);

        LayerFileEntry entry{
            .id = layer.second.id,
            .height = layer.second.height,
            .opacity = layer.second.opacity,
            .blendMode = layer.second.blendMode,
            .hidden = layer.second.hidden,
            .locked = layer.second.locked,
            .mask = layer.second.mask,
            .nameLen = static_cast<uint16_t>(layer.second.name.size()),
        };

        // MDB_RESERVE leaves val.mv_data null on failure, so this has to be checked
        // before the memcpy below.
        val.mv_data = nullptr;
        val.mv_size = sizeof(LayerFileEntry) + entry.nameLen;
        rc = mdb_put(txn, dbiLayers, &key, &val, MDB_RESERVE);
        if (rc != MDB_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to reserve %zu bytes for layer %zu: %s", val.mv_size, id,
                         mdb_strerror(rc));
            SDL_assert(false && "Failed to reserve space for a layer record");
            mdb_txn_abort(txn);
            return Error::Unknown;
        }

        std::memcpy(val.mv_data, &entry, sizeof(LayerFileEntry));
        std::memcpy(reinterpret_cast<std::byte*>(val.mv_data) + sizeof(LayerFileEntry), layer.second.name.c_str(),
                    entry.nameLen);
    }

    rc = mdb_txn_commit(txn);
    if (rc != MDB_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to commit the save transaction: %s", mdb_strerror(rc));
        SDL_assert(false && "Failed to commit the lmdb save transaction");
        return Error::Unknown;
    }

    // Only clear the dirty flags once the data is actually durable, so a failed
    // save is retried instead of silently dropped.
    for (const auto& layer : layers) {
        layer.second.modified = false;
    }

    return Error::None;
}

Error CanvasFile::SaveAs(std::filesystem::path newFilename) {
    ZoneScoped;

    if (std::filesystem::exists(newFilename)) {
        if (std::filesystem::is_regular_file(newFilename)) {
            std::filesystem::remove(newFilename);
        }
    } else {
        std::filesystem::create_directories(newFilename.parent_path());
    }

    if (env) {
        const auto newFilenameStr = newFilename.string();
        mdb_env_copy2(env, newFilenameStr.c_str(), MDB_CP_COMPACT);

        mdb_dbi_close(env, dbiMetadata);
        mdb_dbi_close(env, dbiLayers);
        mdb_dbi_close(env, dbiTiles);
        mdb_env_close(env);
    }

    filename = std::move(newFilename);

    mdb_env_create(&env);
    mdb_env_set_maxdbs(env, 3); // metadata, layers, tiles
    mdb_env_set_mapsize(env, 1_GiB);
    const auto filenameStr = filename->string();
    mdb_env_open(env, filenameStr.c_str(), MDB_NOSUBDIR, 0664);

    MDB_txn* txn{};
    MDB_val key, val;
    mdb_txn_begin(env, nullptr, 0, &txn);
    mdb_dbi_open(txn, "metadata", MDB_CREATE, &dbiMetadata);
    mdb_dbi_open(txn, "layers", MDB_INTEGERKEY | MDB_CREATE, &dbiLayers);
    mdb_dbi_open(txn, "tiles", MDB_INTEGERKEY | MDB_CREATE, &dbiTiles);
    mdb_txn_commit(txn);

    return Save();
}

Error CanvasFile::Close() {
    ZoneScoped;
    SDL_assert(env);

    mdb_dbi_close(env, dbiTiles);
    mdb_dbi_close(env, dbiLayers);
    mdb_dbi_close(env, dbiMetadata);
    mdb_env_close(env);

    env = nullptr;
    filename = std::nullopt;

    SyncCanvasViews();

    return Error::None;
}

Error CanvasFile::Update() {
    ZoneScoped;

    // TODO: Move to multithreaded pool

    MDB_val key{};
    MDB_val val{};
    MDB_txn* txn{};

    // Update tile writing
    if (!tilesWriting.empty()) {
        ZoneScopedN("Update Tile writing");
        if (int rc = mdb_txn_begin(env, nullptr, 0, &txn); !rc) {
            SDL_assert(false && "Failed to begin lmdb write transaction");
        }

        std::vector<Tile, arena_allocator<Tile>> tilesWrote(app->frameArena);
        std::vector<Tile, arena_allocator<Tile>> tilesDeleting(app->frameArena);
        for (auto [tile, data] : tilesWriting) {
            ZoneScopedN("Tile writing");
            SDL_assert(tilesModified.contains(tile));

            if (data.state == TileWriteState::Pending) {
                const auto slot = app->renderer.DownloadTileTexture(tile);
                if (!slot.has_value()) {
                    continue;
                }
                data.rendererSlot = slot.value();
                data.state = TileWriteState::Downloading;
            }
            if (data.state == TileWriteState::Downloading) {
                if (!app->renderer.QueryTileTextureDownload(data.rendererSlot)) {
                    continue;
                }
                const auto& info = app->renderer.GetTileTextureDownloadInfo(data.rendererSlot);
                bool empty{true};
                for (size_t i = 0; i < TILE_SIZE; i++) {
                    if (static_cast<uint8_t>(info.data[i]) != 0) {
                        empty = false;
                    }
                }

                if (empty) {
                    app->renderer.FreeTileTextureDownload(data.rendererSlot);
                    tilesDeleting.push_back(tile);
                    data.state = TileWriteState::Done;
                } else {
                    data.state = TileWriteState::Encoding;
                    const qoi_desc desc = {
                        .width = TILE_WIDTH,
                        .height = TILE_HEIGHT,
                        .channels = 4,
                        .colorspace = QOI_LINEAR,
                    };
                    int encodedBufSize{};
                    auto* encodedBuf = static_cast<std::byte*>(qoi_encode(info.data, &desc, &encodedBufSize));
                    SDL_assert(encodedBufSize > 0);
                    SDL_assert(encodedBuf);

                    data.state = TileWriteState::Writting;
                    key.mv_data = std::bit_cast<size_t*>(&tile);
                    key.mv_size = sizeof(size_t);
                    val.mv_data = encodedBuf;
                    val.mv_size = encodedBufSize;
                    if (int rc = mdb_put(txn, dbiTiles, &key, &val, 0); !rc) {
                        SDL_assert(false && "Failed to write to lmdb tile database");
                    }
                    app->renderer.FreeTileTextureDownload(data.rendererSlot);
                    Free(encodedBuf);

                    layers.at(tile.layer).tiles.insert(tile);
                    data.state = TileWriteState::Done;
                }
                if (data.state == TileWriteState::Done) {
                    tilesModified.erase(tile);
                    tilesWrote.push_back(tile);
                }
            }
        }
        if (!mdb_txn_commit(txn)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write tiles textures. Retrying next frame...");
            tilesWrote.clear();
        }
        for (const auto tile : tilesWrote) {
            tilesWriting.erase(tile);
        }
        for (const auto tile : tilesDeleting) {
            DeleteTile(tile);
        }
    }

    // Update tile reading
    if (!tilesReading.empty()) {
        ZoneScopedN("Update Tile reading");
        // FIXME use mdb_txn_renew instead
        if (int rc = mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn); !rc) {
            SDL_assert(false && "Failed to begin a read only lmdb operation");
        }

        std::vector<Tile, arena_allocator<Tile>> tilesRead(app->frameArena);
        for (auto [tile, data] : tilesReading) {
            ZoneScopedN("Tile reading");
            SDL_assert(!tilesLoaded.contains(tile));

            if (data.state == TileReadState::Pending) {
                const auto slot = app->renderer.ReserveTileTextureUpload(tile);
                if (!slot.has_value()) {
                    continue;
                }
                data.rendererSlot = slot.value();
                const auto& info = app->renderer.GetTileTextureUploadInfo(data.rendererSlot);

                key.mv_data = std::bit_cast<size_t*>(&tile);
                key.mv_size = sizeof(size_t);
                if (int rc = mdb_get(txn, dbiTiles, &key, &val); !rc) {
                    SDL_assert(false && "Failed to read tile");
                    continue;
                }
                data.state = TileReadState::Read;

                qoi_desc desc{};
                int size = static_cast<int>(val.mv_size);
                auto* decodedBuf = static_cast<std::byte*>(qoi_decode(val.mv_data, size, &desc, 4));
                SDL_assert(decodedBuf);
                SDL_assert(desc.width == TILE_WIDTH);
                SDL_assert(desc.height == TILE_HEIGHT);
                SDL_assert(desc.channels = 4);

                data.state = TileReadState::Decoded;

                std::memcpy(info.data, decodedBuf, TILE_SIZE);
                Free(decodedBuf);

                app->renderer.CommitTileTextureUpload(data.rendererSlot);
                data.state = TileReadState::Uploaded;
                tilesLoaded.insert(tile);
                tilesRead.push_back(tile);
                data.state = TileReadState::Done;
            }
        }
        if (!mdb_txn_commit(txn)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write tiles textures. Retrying next frame...");
            tilesRead.clear();
        }
        for (const auto tile : tilesRead) {
            tilesReading.erase(tile);
        }
    }

    return Error::None;
}

Error CanvasFile::Quit() {
    ZoneScoped;
    return Close();
}

bool CanvasFile::HasLayer(Layer layer) const {
    ZoneScoped;
    return layers.contains(layer);
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

    if (!app->renderer.CreateLayerTexture(createInfo.id.value())) {
        layers.erase(createInfo.id.value());
        return std::unexpected(Error::Unknown);
    }

    layersTree.push_back(createInfo.id.value());
    layers.at(createInfo.id.value()).height = static_cast<LayerHeight>(layersTree.size() - 1);
    SetLayerHeight(createInfo.id.value(), createInfo.height);

    layers.at(createInfo.id.value()).tiles.reserve(TILES_MAX);

    SyncCanvasViews();

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

    for (const Tile& tile : layers.at(layer).tiles) {
        tilesModified.erase(tile);
        tilesLoaded.erase(tile);
    }

    SetLayerHeight(layer, static_cast<LayerHeight>(layersTree.size() - 1));
    layersTree.pop_back();

    app->renderer.DestroyLayerTexture(layer);
    layers.erase(layer);

    SyncCanvasViews();

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

    SyncCanvasViews();

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
        std::rotate(begin + oldHeight, begin + oldHeight + 1, begin + target + 1);
    } else {
        std::rotate(begin + target, begin + oldHeight, begin + oldHeight + 1);
    }

    for (size_t i = std::min(oldHeight, target); i <= std::max(oldHeight, target); i++) {
        LayerInfo& info = layers.at(layersTree[i]);
        info.height = static_cast<LayerHeight>(i);
        info.modified = true;
    }

    SyncCanvasViews();

    return Error::None;
}

Error CanvasFile::SetBlendMode(Layer layer, BlendMode newBlendMode) {
    ZoneScoped;

    if (!layers.contains(layer)) {
        return Error::LayerNotFound;
    }

    LayerInfo& info = layers.at(layer);
    info.blendMode = newBlendMode;
    info.modified = true;

    SyncCanvasViews();

    return Error::None;
}

Error CanvasFile::MergeLayers(Layer srcLayer, Layer dstLayer) {
    ZoneScoped;
    SDL_assert(layers.contains(srcLayer));
    SDL_assert(layers.contains(dstLayer));

    // Batch for better result

    for (const auto& srcTile : layers.at(srcLayer).tiles) {
        Tile dstTile = srcTile;
        dstTile.layer = dstLayer;

        MergeTiles(srcTile, dstTile);
    }

    SyncCanvasViews();

    return Error::None;
}

std::expected<Layer, Error> CanvasFile::DuplicateLayer(Layer layer) {
    ZoneScoped;
    SDL_assert(false && "TODO!");

    // TODO DuplicateLayer

    SyncCanvasViews();

    return std::unexpected(Error::Unknown);
}

bool CanvasFile::HasTile(Tile tile) const {
    ZoneScoped;

    if (!layers.contains(tile.layer)) {
        return false;
    }
    return layers.at(tile.layer).tiles.contains(tile);
}

Error CanvasFile::NewTile(Tile tile) {
    ZoneScoped;
    SDL_assert(layers.contains(tile.layer) && "Layer missing");
    SDL_assert(!tilesLoaded.contains(tile) && "Tile already loaded");
    SDL_assert(!layers.at(tile.layer).tiles.contains(tile) && "Tile already exists");

    // Batch for better perfs

    if (auto err = app->renderer.CreateTileTexture(tile); err != Error::None) {
        SDL_assert(err != Error::None && "Failed to create gpu texture");
        return Error::Unknown;
    }

    tilesLoaded.insert(tile);
    MarkTileDirty(tile);

    SyncCanvasViews();

    return Error::None;
}

Error CanvasFile::DeleteTile(Tile tile) {
    ZoneScoped;
    SDL_assert(env);

    // TODO: figure tileLoading
    // TODO: figure tileUnloading

    if (!layers.at(tile.layer).tiles.contains(tile)) {
        return Error::TileNotFound;
    }

    SDL_assert(!tilesReading.contains(tile) && "Haven't figured this out yet :/");

    // Batch for better perfs
    MDB_txn* txn{};
    MDB_val key{};
    key.mv_data = &tile;
    key.mv_size = sizeof(size_t);
    mdb_txn_begin(env, nullptr, 0, &txn);
    mdb_del(txn, dbiTiles, &key, nullptr);
    mdb_txn_commit(txn);

    tilesModified.erase(tile);
    layers.at(tile.layer).tiles.erase(tile);

    return Error::None;
}

Error CanvasFile::DestroyTile(Tile tile) {
    ZoneScoped;
    SDL_assert(tilesLoaded.contains(tile));

    app->renderer.DestroyTileTexture(tile);
    tilesLoaded.erase(tile);

    SyncCanvasViews();

    return Error::None;
}

Error CanvasFile::MarkTileDirty(Tile tile) {
    ZoneScoped;

    if (!layers.contains(tile.layer)) {
        return Error::TileNotFound;
    }
    if (layers.at(tile.layer).internal) {
        return Error::None;
    }

    tilesModified.insert(tile);

    return Error::None;
}

Error CanvasFile::ReadTile(Tile tile) {
    ZoneScoped;
    SDL_assert(layers.at(tile.layer).tiles.contains(tile) && "Tile not saved, cannot load it");

    // FIXME This needs to be thread safe

    if (tilesWriting.contains(tile) && app->renderer.tileTextures.contains(tile)) {
        tilesWriting.erase(tile);
        tilesLoaded.insert(tile);
        return Error::None;
    }

    if (tilesReading.contains(tile)) {
        return Error::None;
    }
    if (app->renderer.CreateUninitializedTileTexture(tile) != Error::None) {
        return Error::Unknown;
    }
    const TileReadData data{
        .tile = tile,
        .state = TileReadState::Pending,
    };

    if (!tilesReading.emplace(tile, data)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed queue tile(%u, %d, %d) for reading", tile.layer, tile.x,
                    tile.y);
        return Error::OutOfCapacity;
    }

    return Error::None;
}

Error CanvasFile::WriteTile(Tile tile) {
    ZoneScoped;

    // FIXME This needs to be thread safe

    if (!tilesLoaded.contains(tile)) {
        // Sanity check
        return Error::TileNotFound;
    }

    if (!tilesModified.contains(tile)) {
        return Error::None;
    }
    if (tilesWriting.contains(tile)) {
        // FIXME The tile may have changed in between
        return Error::None;
    }

    const TileWriteData data{
        .tile = tile,
        .state = TileWriteState::Pending,
    };
    if (!tilesWriting.insert(tile, data)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed queue tile(%u, %d, %d) for saving", tile.layer, tile.x,
                    tile.y);
        return Error::OutOfCapacity;
    }

    return Error::None;
}

Error CanvasFile::MergeTiles(Tile srcTile, Tile dstTile) {
    ZoneScoped;
    SDL_assert(layers.contains(srcTile.layer));
    SDL_assert(layers.at(srcTile.layer).tiles.contains(srcTile));

    // FIXME handling non resident tiles tile is broken
    // TODO fix the path when tile are not visible

    if (tilesLoaded.contains(srcTile)) {
        if (tilesLoaded.contains(dstTile)) {
            app->renderer.MergeTileTextures(srcTile, dstTile);
            MarkTileDirty(dstTile);
        } else {
            MoveTile(srcTile, dstTile);
        }
    } else {
        ReadTile(srcTile);
        if (!tilesLoaded.contains(dstTile) && layers.at(dstTile.layer).tiles.contains(dstTile)) {
            ReadTile(dstTile);
        }

        // TODO wait for the readTile
        MergeTiles(srcTile, dstTile);
    }

    SyncCanvasViews();

    return Error::None;
}

Error CanvasFile::MoveTile(Tile srcTile, Tile dstTile) {
    ZoneScoped;

    SDL_assert(layers.contains(srcTile.layer));
    SDL_assert(layers.contains(dstTile.layer));

    if (tilesLoaded.contains(srcTile)) {
        tilesLoaded.insert(dstTile);
        MarkTileDirty(dstTile);

        const auto& tileTexture = app->renderer.tileTextures.at(srcTile);
        app->renderer.tileTextures.insert(dstTile, tileTexture);
        app->renderer.tileTextures.erase(srcTile);
        tilesLoaded.erase(srcTile);
    }
    if (layers.at(srcTile.layer).tiles.contains(srcTile)) {
        if (auto err = DeleteTile(srcTile); err != Error::None) {
            return err;
        }
    }

    SyncCanvasViews();

    return Error::None;
}

Error CanvasFile::DuplicateTile(Tile srcTile, Tile dstTile) {
    ZoneScoped;

    SyncCanvasViews();

    return Error::None;
}

void CanvasFile::SyncCanvasViews() {
    ZoneScoped;
    for (const auto& id : canvasViewIds) {
        if (!id.has_value()) {
            continue;
        }

        SDL_assert(app->canvasViews.contains(id.value()));
        auto& canvasView = app->canvasViews.at(id.value());
        canvasView.dirty = true;
    }
}

//
// CanvasView
//

CanvasView::CanvasView(App* app, size_t windowId, size_t canvasFileId)
    : app(app), windowId(windowId), canvasFileId(canvasFileId) {
    SDL_assert(app->canvasFiles.contains(canvasFileId));

    auto& canvasFile = app->canvasFiles.at(canvasFileId);
}

CanvasView::~CanvasView() = default;

Error CanvasView::Init() {
    ZoneScoped;
    return Error::None;
}

Error CanvasView::Update() {
    ZoneScoped;
    auto& canvasFile = app->canvasFiles.at(canvasFileId);

    if (dirty) {
        // ??? rerender the whole screen ?
        dirty = false;
    }

    const auto& visibleTiles = viewport.VisibleTiles(app->frameArena);
    for (const auto& loadedTile : canvasFile.tilesLoaded) {
        if (canvasFile.layers.at(loadedTile.layer).hidden || !viewport.IsTileVisible({loadedTile.x, loadedTile.y})) {
            UnloadTile(loadedTile);
        }
    }

    for (const auto& layer : canvasFile.layersTree) {
        auto& layerInfo = canvasFile.layers.at(layer);
        for (const auto& visibleTile : visibleTiles) {
            const Tile tile{
                .layer = layer,
                .y = static_cast<TileAxis>(visibleTile.y),
                .x = static_cast<TileAxis>(visibleTile.x),
            };

            if (!canvasFile.tilesLoaded.contains(tile) && layerInfo.tiles.contains(tile)) {
                LoadTile(tile);
            }
        }
    }

    return Error::None;
}

Error CanvasView::Render() {
    ZoneScoped;
    return Error::None;
}

Error CanvasView::Quit() {
    ZoneScoped;
    return Error::None;
}

Error CanvasView::Resize(int newWidth, int newHeight) {
    ZoneScoped;
    return Error::None;
}

Error CanvasView::Move(int newX, int newY) {
    ZoneScoped;
    return Error::None;
}

Error CanvasView::HideLayer(Layer layer, bool hide) {
    ZoneScoped;
    return Error::None;
}

Error CanvasView::LockLayer(Layer layer, bool lock) {
    ZoneScoped;
    return Error::None;
}

Error CanvasView::SelectLayer(Layer layer) {
    ZoneScoped;
    return Error::None;
}

Error CanvasView::LoadTile(Tile tile) {
    ZoneScoped;
    return Error::None;
}

Error CanvasView::UnloadTile(Tile tile) {
    ZoneScoped;
    return Error::None;
}

} // namespace Midori
