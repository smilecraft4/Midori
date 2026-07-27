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

CanvasFile::CanvasFile(App* app) : app(app) {
    ZoneScoped;
}

CanvasFile::~CanvasFile() = default;

Error CanvasFile::Init(size_t newId) {
    id = newId;
    return Error::None;
}

Error CanvasFile::New() {
    ZoneScoped;
    SDL_assert(!env && "New() on a CanvasFile that still owns an lmdb env; Close() first");
    SDL_assert(!filename.has_value() && "New() on a CanvasFile that still has a filename");

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
    SDL_assert(!env && "Open() on a CanvasFile that still owns an lmdb env; Close() first");
    SDL_assert(newFilename.has_filename() && "Open() needs a file path, not a directory");

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
        UnloadTile(tileModified);
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

    const auto filenameStr = newFilename.string();

    if (env) {
        // Copy the live env to the new path, then reopen there so subsequent writes
        // land in the new file. Keep the old env open if the copy fails.
        int rc = mdb_env_copy2(env, filenameStr.c_str(), MDB_CP_COMPACT);
        if (rc != MDB_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to copy the lmdb env to '%s': %s", filenameStr.c_str(),
                         mdb_strerror(rc));
            SDL_assert(false && "Failed to copy the lmdb environment");
            return Error::Unknown;
        }

        mdb_dbi_close(env, dbiMetadata);
        mdb_dbi_close(env, dbiLayers);
        mdb_dbi_close(env, dbiTiles);
        mdb_env_close(env);
        env = nullptr;
        filename = std::nullopt;
    }

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

    filename = std::move(newFilename);

    return Save();
}

Error CanvasFile::Close() {
    ZoneScoped;
    SDL_assert(env && "Close() on a CanvasFile with no lmdb env open");
    if (!env) {
        return Error::None;
    }

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
        if (int rc = mdb_txn_begin(env, nullptr, 0, &txn); rc != MDB_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to begin the tile write transaction: %s",
                         mdb_strerror(rc));
            SDL_assert(false && "Failed to begin the lmdb tile write transaction");
            return Error::Unknown;
        }

        std::vector<Tile, arena_allocator<Tile>> tilesWrote(app->frameArena);
        std::vector<Tile, arena_allocator<Tile>> tilesDeleting(app->frameArena);
        for (auto [tile, data] : tilesWriting) {
            ZoneScopedN("Tile writing");
            SDL_assert(tilesModified.contains(tile) && "A tile queued for writing must still be marked modified");

            if (data.state == TileWriteState::Pending) {
                const auto slot = app->renderer.DownloadTileTexture(*this, tile);
                if (!slot.has_value()) {
                    continue;
                }
                data.rendererSlot = slot.value();
                data.state = TileWriteState::Downloading;
            }
            if (data.state == TileWriteState::Downloading) {
                if (!app->renderer.QueryTileTextureDownload(*this, data.rendererSlot)) {
                    continue;
                }
                const auto& info = app->renderer.GetTileTextureDownloadInfo(*this, data.rendererSlot);
                bool empty{true};
                for (size_t i = 0; i < TILE_SIZE; i++) {
                    if (static_cast<uint8_t>(info.data[i]) != 0) {
                        empty = false;
                    }
                }

                if (empty) {
                    app->renderer.FreeTileTextureDownload(*this, data.rendererSlot);
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
                    SDL_assert(encodedBuf && "qoi_encode returned no buffer");
                    SDL_assert(encodedBufSize > 0 && "qoi_encode produced an empty buffer");
                    if (encodedBuf == nullptr || encodedBufSize <= 0) {
                        // Leave the tile queued so the next frame retries it.
                        app->renderer.FreeTileTextureDownload(*this, data.rendererSlot);
                        data.state = TileWriteState::Pending;
                        continue;
                    }

                    data.state = TileWriteState::Writting;
                    key.mv_data = std::bit_cast<size_t*>(&tile);
                    key.mv_size = sizeof(Tile);
                    val.mv_data = encodedBuf;
                    val.mv_size = encodedBufSize;
                    const int rc = mdb_put(txn, dbiTiles, &key, &val, 0);

                    app->renderer.FreeTileTextureDownload(*this, data.rendererSlot);
                    Free(encodedBuf);

                    if (rc != MDB_SUCCESS) {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write tile(%u, %d, %d): %s", tile.layer,
                                     tile.x, tile.y, mdb_strerror(rc));
                        SDL_assert(false && "Failed to write to the lmdb tile database");
                        data.state = TileWriteState::Pending;
                        continue;
                    }

                    if (layers.at(tile.layer).tiles.full()) {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Layer %u is at its tile limit", tile.layer);
                    } else {
                        layers.at(tile.layer).tiles.insert(tile);
                    }
                    data.state = TileWriteState::Done;
                }
                if (data.state == TileWriteState::Done) {
                    tilesModified.erase(tile);
                    tilesWrote.push_back(tile);
                }
            }
        }
        if (int rc = mdb_txn_commit(txn); rc != MDB_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to commit %zu tile textures, retrying next frame: %s",
                         tilesWrote.size(), mdb_strerror(rc));

            // Nothing reached the file, so put every tile back in the queue. The
            // download slots are already released, hence the reset to Pending.
            for (const auto tile : tilesWrote) {
                tilesModified.insert(tile);
                tilesWriting.at(tile).state = TileWriteState::Pending;
            }
            tilesWrote.clear();
            tilesDeleting.clear();
        }
        for (const auto tile : tilesWrote) {
            tilesWriting.erase(tile);
        }
        // Deleted outside the transaction above: DeleteTile() opens its own write
        // transaction and lmdb deadlocks if a thread nests two of them.
        for (const auto tile : tilesDeleting) {
            DeleteTile(tile);
            DestroyTile(tile);
        }
    }

    // Update tile reading
    if (!tilesReading.empty()) {
        ZoneScopedN("Update Tile reading");
        // FIXME use mdb_txn_renew instead
        if (int rc = mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn); rc != MDB_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to begin the tile read transaction: %s",
                         mdb_strerror(rc));
            SDL_assert(false && "Failed to begin the lmdb tile read transaction");
            return Error::Unknown;
        }

        std::vector<Tile, arena_allocator<Tile>> tilesRead(app->frameArena);
        for (auto [tile, data] : tilesReading) {
            ZoneScopedN("Tile reading");
            SDL_assert(!tilesLoaded.contains(tile) && "A tile queued for reading must not already be loaded");

            if (data.state == TileReadState::Pending) {
                const auto slot = app->renderer.ReserveTileTextureUpload(*this, tile);
                if (!slot.has_value()) {
                    continue;
                }
                data.rendererSlot = slot.value();
                const auto& info = app->renderer.GetTileTextureUploadInfo(*this, data.rendererSlot);

                key.mv_data = std::bit_cast<size_t*>(&tile);
                key.mv_size = sizeof(Tile);
                if (int rc = mdb_get(txn, dbiTiles, &key, &val); rc != MDB_SUCCESS) {
                    // MDB_NOTFOUND means the layer's tile set and the tile database
                    // disagree; either way this read can never complete, so drop it
                    // rather than retrying it every frame.
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read tile(%u, %d, %d): %s", tile.layer,
                                 tile.x, tile.y, mdb_strerror(rc));
                    SDL_assert(false && "Failed to read a tile from the lmdb tile database");
                    app->renderer.FreeTileTextureUpload(*this, data.rendererSlot);
                    tilesRead.push_back(tile);
                    continue;
                }
                data.state = TileReadState::Read;

                qoi_desc desc{};
                int size = static_cast<int>(val.mv_size);
                auto* decodedBuf = static_cast<std::byte*>(qoi_decode(val.mv_data, size, &desc, 4));
                SDL_assert(decodedBuf && "qoi_decode returned no buffer");
                if (decodedBuf == nullptr || desc.width != TILE_WIDTH || desc.height != TILE_HEIGHT
                    || desc.channels != 4) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "Tile(%u, %d, %d) decoded to %ux%u/%uch, expected %zux%zu/4ch", tile.layer, tile.x,
                                 tile.y, desc.width, desc.height, desc.channels, TILE_WIDTH, TILE_HEIGHT);
                    SDL_assert(false && "Decoded tile has unexpected dimensions");
                    Free(decodedBuf);
                    app->renderer.FreeTileTextureUpload(*this, data.rendererSlot);
                    tilesRead.push_back(tile);
                    continue;
                }

                data.state = TileReadState::Decoded;

                std::memcpy(info.data, decodedBuf, TILE_SIZE);
                Free(decodedBuf);

                app->renderer.CommitTileTextureUpload(*this, data.rendererSlot);
                data.state = TileReadState::Uploaded;
                tilesLoaded.insert(tile);
                tilesRead.push_back(tile);
                data.state = TileReadState::Done;
            }
        }
        // Read-only transactions have nothing to commit.
        mdb_txn_abort(txn);

        for (const auto tile : tilesRead) {
            tilesReading.erase(tile);
        }
    }

    return Error::None;
}

// TODO this stuff is bugous, buffy, and weird. It needs to be fixed
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

    for (const auto& tile : tilesLoaded) {
        if (tile.layer == layer) {
            DestroyTile(tile);
        }
    }

    for (const Tile& tile : layers.at(layer).tiles) {
        tilesModified.erase(tile);
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
    SDL_assert(layers.contains(srcLayer) && "Merge source layer does not exist");
    SDL_assert(layers.contains(dstLayer) && "Merge destination layer does not exist");

    // Batch for better result
    size_t failed{};
    for (const auto& tile : tilesLoaded) {
        if (tile.layer == srcLayer) {
            Tile target{
                .layer = dstLayer,
                .y = tile.y,
                .x = tile.x,
            };
            if (auto err = MergeTiles(tile, target); err != Error::None) {
                failed++;
            }
        }
    }

    // TODO find a way to handle non loaded tiles.
    // for example make LoadTileAsync and LoadTile for immediate stuff
    // the same for UnloadTileAsync and UnloadTile for immediate stuff

    SyncCanvasViews();

    return failed == 0 ? Error::None : Error::Unknown;
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
    SDL_assert(layers.contains(tile.layer) && "NewTile() on a layer that does not exist");
    SDL_assert(!tilesLoaded.contains(tile) && "NewTile() on a tile that is already loaded");
    SDL_assert(!layers.at(tile.layer).tiles.contains(tile) && "NewTile() on a tile that already exists in the file");

    // Batch for better perfs

    if (auto err = app->renderer.CreateTileTexture(*this, tile); err != Error::None) {
        SDL_assert(false && "Failed to create the tile gpu texture");
        return Error::Unknown;
    }

    tilesLoaded.insert(tile);
    MarkTileDirty(tile);

    SyncCanvasViews();

    return Error::None;
}

Error CanvasFile::DeleteTile(Tile tile) {
    ZoneScoped;
    SDL_assert(env && "DeleteTile() needs an open lmdb env");

    // TODO: figure tileLoading
    // TODO: figure tileUnloading

    if (!layers.at(tile.layer).tiles.contains(tile)) {
        return Error::TileNotFound;
    }
    if (!layers.contains(tile.layer)) {
        return Error::LayerNotFound;
    }

    SDL_assert(!tilesReading.contains(tile) && "Deleting a tile that is mid-read is not handled yet");

    // Batch for better perfs
    MDB_txn* txn{};
    MDB_val key{};
    key.mv_data = &tile;
    key.mv_size = sizeof(Tile);

    int rc = mdb_txn_begin(env, nullptr, 0, &txn);
    if (rc != MDB_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to begin the tile delete transaction: %s", mdb_strerror(rc));
        SDL_assert(false && "Failed to begin the lmdb tile delete transaction");
        return Error::Unknown;
    }

    rc = mdb_del(txn, dbiTiles, &key, nullptr);
    if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to delete tile(%u, %d, %d): %s", tile.layer, tile.x, tile.y,
                     mdb_strerror(rc));
        SDL_assert(false && "Failed to delete a tile from the lmdb tile database");
        mdb_txn_abort(txn);
        return Error::Unknown;
    }

    rc = mdb_txn_commit(txn);
    if (rc != MDB_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to commit the tile delete transaction: %s",
                     mdb_strerror(rc));
        SDL_assert(false && "Failed to commit the lmdb tile delete transaction");
        return Error::Unknown;
    }

    tilesModified.erase(tile);
    layers.at(tile.layer).tiles.erase(tile);

    return Error::None;
}

Error CanvasFile::DestroyTile(Tile tile) {
    ZoneScoped;
    if (!tilesLoaded.contains(tile)) {
        return Error::TileNotFound;
    }

    app->renderer.DestroyTileTexture(*this, tile);
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

Error CanvasFile::LoadTile(Tile tile) {
    ZoneScoped;
    SDL_assert(layers.contains(tile.layer) && "ReadTile() on a layer that does not exist");
    SDL_assert(HasTile(tile) && "ReadTile() on a tile that was never written to the file");

    // FIXME This needs to be thread safe

    if (tilesWriting.contains(tile) && app->renderer.tileTextures.contains(tile)) {
        tilesWriting.erase(tile);
        tilesLoaded.insert(tile);
        return Error::None;
    }

    if (tilesReading.contains(tile)) {
        return Error::None;
    }
    if (app->renderer.CreateUninitializedTileTexture(*this, tile) != Error::None) {
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

Error CanvasFile::UnloadTile(Tile tile) {
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
    SDL_assert(layers.contains(srcTile.layer) && "Merge source layer does not exist");
    SDL_assert(tilesLoaded.contains(srcTile) && "Tile not loaded");

    // FIXME handling non resident tiles tile is broken
    // TODO fix the path when tile are not visible

    const Renderer::MergeInfo mergeInfo{
        .blendMode = layers.at(srcTile.layer).blendMode,
        .opacity = layers.at(srcTile.layer).opacity,
    };
    if (tilesLoaded.contains(dstTile)) {
        app->renderer.MergeTileTextures(*this, srcTile, dstTile, mergeInfo);
        MarkTileDirty(dstTile);
    } else {
        MoveTile(srcTile, dstTile);
    }
    SyncCanvasViews();

    return Error::None;
}

Error CanvasFile::MoveTile(Tile srcTile, Tile dstTile) {
    ZoneScoped;

    SDL_assert(layers.contains(srcTile.layer) && "Move source layer does not exist");
    SDL_assert(layers.contains(dstTile.layer) && "Move destination layer does not exist");

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

        SDL_assert(app->canvasViews.contains(id.value())
                   && "CanvasFile references a canvas view that no longer exists");
        auto& canvasView = app->canvasViews.at(id.value());
        canvasView.dirty = true;
    }
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

    viewport.ComputeViewMatrix();
    const auto& visibleTiles = viewport.VisibleTiles(app->frameArena);
    for (const auto& loadedTile : canvasFile.tilesLoaded) {
        if (canvasFile.layers.at(loadedTile.layer).hidden || !viewport.IsTileVisible({loadedTile.x, loadedTile.y})) {
            canvasFile.UnloadTile(loadedTile);
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
                canvasFile.LoadTile(tile);
            }
        }
    }

    return Error::None;
}

Error CanvasView::Quit() {
    ZoneScoped;
    auto& canvasFile = app->canvasFiles.at(canvasFileId);

    const auto& visibleTiles = viewport.VisibleTiles(app->frameArena);
    for (const auto& loadedTile : canvasFile.tilesLoaded) {
        if (canvasFile.layers.at(loadedTile.layer).hidden || !viewport.IsTileVisible({loadedTile.x, loadedTile.y})) {
            canvasFile.UnloadTile(loadedTile);
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
