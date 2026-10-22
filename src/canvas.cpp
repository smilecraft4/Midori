#include "midori/canvas.hpp"

#include "midori/app.hpp"
#include "midori/graphics.hpp"
#include "midori/log.hpp"
#include "midori/tile.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include <mdbx.h>
#include <tracy/Tracy.hpp>

namespace Midori {

namespace {

constexpr const char* kMetaTable = "meta";
constexpr const char* kLayersTable = "layers";
constexpr const char* kTilesTable = "tiles";
// Large growth steps keep remapping rare; the cap only bounds the address space reserved for the map.
constexpr intptr_t kFileGrowth = intptr_t{64} << 20;
constexpr intptr_t kFileSizeMax = intptr_t{1} << 40;

void LogMdbx(const char* what, int rc) noexcept {
    MIDORI_LOG_ERROR("{} failed: {}", what, mdbx_strerror(rc));
}

template<typename T>
[[nodiscard]] MDBX_val Val(const T& value) noexcept {
    return MDBX_val{.iov_base = const_cast<T*>(&value), .iov_len = sizeof(T)};
}

[[nodiscard]] std::filesystem::path LockPath(const std::filesystem::path& path) noexcept {
    std::filesystem::path lock = path;
    lock += MDBX_LOCK_SUFFIX_W;
    return lock;
}

// libmdbx keeps the lock file even in exclusive mode; nothing else can be using it once the env is closed.
void CanvasFileClose(CanvasFile& file) noexcept {
    if (file.env) {
        const wchar_t* path = nullptr;
        std::filesystem::path lock;
        if (mdbx_env_get_pathW(file.env, &path) == MDBX_SUCCESS) {
            lock = LockPath(path);
        }
        mdbx_env_close(file.env);
        std::error_code ec;
        std::filesystem::remove(lock, ec);
    }
    file = {};
}

// Exclusive: another process cannot open the file while it is ours.
[[nodiscard]] Result<CanvasFile> CanvasFileOpen(const std::filesystem::path& path, bool create, Error error,
                                                MDBX_env_flags_t envFlags) noexcept {
    ZoneScopedN("CanvasFileOpen");
    CanvasFile file{};
    const auto fail = [&file, error](const char* what, int rc) {
        LogMdbx(what, rc);
        CanvasFileClose(file);
        return std::unexpected(error);
    };
    if (const int rc = mdbx_env_create(&file.env); rc != MDBX_SUCCESS) {
        file.env = nullptr;
        return fail("mdbx_env_create", rc);
    }
    if (const int rc = mdbx_env_set_maxdbs(file.env, 3); rc != MDBX_SUCCESS) {
        return fail("mdbx_env_set_maxdbs", rc);
    }
    if (const int rc = mdbx_env_set_geometry(file.env, -1, -1, kFileSizeMax, kFileGrowth, -1, -1); rc != MDBX_SUCCESS) {
        return fail("mdbx_env_set_geometry", rc);
    }
    // Mode 0 opens an existing file only.
    if (const int rc = mdbx_env_openW(file.env, path.c_str(), MDBX_NOSUBDIR | MDBX_EXCLUSIVE | envFlags,
                                      static_cast<mdbx_mode_t>(create ? 0644 : 0));
        rc != MDBX_SUCCESS) {
        return fail("mdbx_env_open", rc);
    }

    MDBX_txn* txn = nullptr;
    if (const int rc = mdbx_txn_begin(file.env, nullptr, MDBX_TXN_READWRITE, &txn); rc != MDBX_SUCCESS) {
        return fail("mdbx_txn_begin", rc);
    }
    const MDBX_db_flags_t flags = create ? MDBX_INTEGERKEY | MDBX_CREATE : MDBX_INTEGERKEY;
    for (const auto& [name, dbi] : {std::pair{kMetaTable, &file.metaDbi}, std::pair{kLayersTable, &file.layersDbi},
                                    std::pair{kTilesTable, &file.tilesDbi}}) {
        if (const int rc = mdbx_dbi_open(txn, name, flags, dbi); rc != MDBX_SUCCESS) {
            mdbx_txn_abort(txn);
            return fail("mdbx_dbi_open", rc);
        }
    }
    if (const int rc = mdbx_txn_commit(txn); rc != MDBX_SUCCESS) {
        return fail("mdbx_txn_commit", rc);
    }
    return file;
}

// A new file in the temp directory. Losing it in a crash loses nothing saved, so it is never synced.
[[nodiscard]] CanvasFile CacheOpen(Canvas canvas) noexcept {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (!ec) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto cache = CanvasFileOpen(dir / std::format("midori-{}-{}.cache", stamp, canvas), true,
                                          Error::TileCacheFailed, MDBX_UTTERLY_NOSYNC);
        if (cache) {
            return *cache;
        }
    }
    MIDORI_LOG_WARN("canvas {}: no tile cache, unsaved tiles keep their atlas cells", canvas);
    return {};
}

void CacheClose(CanvasFile& cache) noexcept {
    std::filesystem::path path;
    const wchar_t* raw = nullptr;
    if (cache.env && mdbx_env_get_pathW(cache.env, &raw) == MDBX_SUCCESS) {
        path = raw;
    }
    CanvasFileClose(cache);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// kTileBytes per key into `pixels`.
[[nodiscard]] Result<void> TilesRead(const CanvasFile& file, std::span<const TileKey> keys,
                                     std::span<std::byte> pixels) noexcept {
    MIDORI_ASSERT(pixels.size() == keys.size() * kTileBytes, "{} bytes for {} tiles", pixels.size(), keys.size());
    if (keys.empty()) {
        return {};
    }
    MDBX_txn* txn = nullptr;
    if (const int rc = mdbx_txn_begin(file.env, nullptr, MDBX_TXN_RDONLY, &txn); rc != MDBX_SUCCESS) {
        LogMdbx("mdbx_txn_begin", rc);
        return std::unexpected(Error::TileReadFailed);
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        MDBX_val key = Val(keys[i]);
        MDBX_val value{};
        const int rc = mdbx_get(txn, file.tilesDbi, &key, &value);
        if (rc != MDBX_SUCCESS || value.iov_len != kTileBytes) {
            MIDORI_LOG_ERROR("reading tile {:#018x}: {}", keys[i],
                             rc == MDBX_SUCCESS ? "invalid size" : mdbx_strerror(rc));
            mdbx_txn_abort(txn);
            return std::unexpected(Error::TileReadFailed);
        }
        std::memcpy(pixels.data() + (i * kTileBytes), value.iov_base, kTileBytes);
    }
    mdbx_txn_abort(txn);
    return {};
}

[[nodiscard]] Result<void> TilesWrite(const CanvasFile& file, std::span<const TileKey> keys,
                                      std::span<const std::byte> pixels) noexcept {
    MIDORI_ASSERT(pixels.size() == keys.size() * kTileBytes, "{} bytes for {} tiles", pixels.size(), keys.size());
    MDBX_txn* txn = nullptr;
    if (const int rc = mdbx_txn_begin(file.env, nullptr, MDBX_TXN_READWRITE, &txn); rc != MDBX_SUCCESS) {
        LogMdbx("mdbx_txn_begin", rc);
        return std::unexpected(Error::TileCacheFailed);
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        MDBX_val key = Val(keys[i]);
        MDBX_val value{.iov_base = const_cast<std::byte*>(pixels.data() + (i * kTileBytes)), .iov_len = kTileBytes};
        if (const int rc = mdbx_put(txn, file.tilesDbi, &key, &value, MDBX_UPSERT); rc != MDBX_SUCCESS) {
            LogMdbx("mdbx_put tile", rc);
            mdbx_txn_abort(txn);
            return std::unexpected(Error::TileCacheFailed);
        }
    }
    if (const int rc = mdbx_txn_commit(txn); rc != MDBX_SUCCESS) {
        LogMdbx("mdbx_txn_commit", rc);
        return std::unexpected(Error::TileCacheFailed);
    }
    return {};
}

// Writes what changed since the last save, or the whole canvas into a new file when `all`. Deletes go
// first, so a tile deleted then created again is written.
[[nodiscard]] Result<void> CanvasFileWrite(CanvasState& canvas, const CanvasFile& file, bool all) noexcept {
    ZoneScopedN("CanvasFileWrite");
    // Tiles in the atlas are downloaded. Evicted ones are read from the cache, or from the canvas's own file when
    // unchanged since its last save.
    std::vector<TileAtlasCell> cells;
    std::vector<TileKey> keys; // the atlas ones, then the cached ones, then the saved ones
    std::vector<TileKey> cachedKeys;
    std::vector<TileKey> savedKeys;
    for (const TileState& tile : canvas.tiles) {
        if (!all && !HasAnyFlag(tile.flags, TileFlags::Modified)) {
            continue;
        }
        if (tile.cell != kTileAtlasCellInvalid) {
            cells.push_back(tile.cell);
            keys.push_back(tile.key);
        } else if (HasAnyFlag(tile.flags, TileFlags::CachedDisk)) {
            cachedKeys.push_back(tile.key);
        } else {
            savedKeys.push_back(tile.key);
        }
    }
    MIDORI_ASSERT(all || savedKeys.empty(), "modified tiles evicted without a cache");
    std::vector<std::byte> pixels((keys.size() + cachedKeys.size() + savedKeys.size()) * kTileBytes);
    const std::span<std::byte> downloaded = std::span(pixels).first(cells.size() * kTileBytes);
    const std::span<std::byte> cached = std::span(pixels).subspan(downloaded.size(), cachedKeys.size() * kTileBytes);
    const std::span<std::byte> saved = std::span(pixels).subspan(downloaded.size() + cached.size());
    if (const auto res = GraphicsTilesDownload(cells, downloaded); !res) {
        return std::unexpected(res.error());
    }
    if (!TilesRead(canvas.cache, cachedKeys, cached) || !TilesRead(canvas.file, savedKeys, saved)) {
        return std::unexpected(Error::CanvasSaveFailed);
    }
    keys.insert(keys.end(), cachedKeys.begin(), cachedKeys.end());
    keys.insert(keys.end(), savedKeys.begin(), savedKeys.end());

    MDBX_txn* txn = nullptr;
    if (const int rc = mdbx_txn_begin(file.env, nullptr, MDBX_TXN_READWRITE, &txn); rc != MDBX_SUCCESS) {
        LogMdbx("mdbx_txn_begin", rc);
        return std::unexpected(Error::CanvasSaveFailed);
    }
    const auto fail = [txn](const char* what, int rc) {
        LogMdbx(what, rc);
        mdbx_txn_abort(txn);
        return std::unexpected(Error::CanvasSaveFailed);
    };
    const auto put = [txn](MDBX_dbi dbi, MDBX_val key, MDBX_val value) {
        return mdbx_put(txn, dbi, &key, &value, MDBX_UPSERT);
    };

    if (!all) {
        for (Layer id = 0; id < canvas.largestLayer; ++id) {
            if (!HasAnyFlag(canvas.layers[id].flags, LayerFlags::Deleted)) {
                continue;
            }
            const std::uint32_t layerKey = id;
            MDBX_val key = Val(layerKey);
            if (const int rc = mdbx_del(txn, file.layersDbi, &key, nullptr);
                rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                return fail("mdbx_del", rc);
            }
            MDBX_cursor* cursor = nullptr;
            if (const int rc = mdbx_cursor_open(txn, file.tilesDbi, &cursor); rc != MDBX_SUCCESS) {
                return fail("mdbx_cursor_open", rc);
            }
            const TileKey first = TileKey{id} << kTileLayerBitOffset;
            int rc = MDBX_SUCCESS;
            for (;;) {
                MDBX_val found = Val(first);
                MDBX_val value{};
                rc = mdbx_cursor_get(cursor, &found, &value, MDBX_SET_RANGE);
                if (rc != MDBX_SUCCESS) {
                    break;
                }
                TileKey foundKey{};
                std::memcpy(&foundKey, found.iov_base, sizeof(foundKey));
                if ((foundKey >> kTileLayerBitOffset) != id) {
                    rc = MDBX_NOTFOUND;
                    break;
                }
                rc = mdbx_cursor_del(cursor, MDBX_CURRENT);
                if (rc != MDBX_SUCCESS) {
                    break;
                }
            }
            mdbx_cursor_close(cursor);
            if (rc != MDBX_NOTFOUND) {
                return fail("deleting layer tiles", rc);
            }
        }
        for (const TileKey tileKey : canvas.tileDeletes) {
            MDBX_val key = Val(tileKey);
            if (const int rc = mdbx_del(txn, file.tilesDbi, &key, nullptr); rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                return fail("mdbx_del", rc);
            }
        }
    }

    const CanvasFileHeader header{
        .magic = kCanvasFileMagic,
        .version = kCanvasFileVersion,
        .largestLayer = canvas.largestLayer,
        .pixelFormat = canvas.pixelFormat,
        .reserved = 0,
    };
    const auto headerKey = std::to_underlying(CanvasFileMeta::Header);
    if (const int rc = put(file.metaDbi, Val(headerKey), Val(header)); rc != MDBX_SUCCESS) {
        return fail("mdbx_put header", rc);
    }
    const auto orderKey = std::to_underlying(CanvasFileMeta::LayersOrder);
    const MDBX_val order{.iov_base = canvas.layersOrder.data(), .iov_len = canvas.layersOrder.size() * sizeof(Layer)};
    if (const int rc = put(file.metaDbi, Val(orderKey), order); rc != MDBX_SUCCESS) {
        return fail("mdbx_put layers order", rc);
    }

    for (const Layer id : canvas.layersOrder) {
        const LayerState& layer = canvas.layers[id];
        if (!all && !HasAnyFlag(layer.flags, LayerFlags::Dirty)) {
            continue;
        }
        const std::uint32_t layerKey = id;
        const LayerFileEntry entry{
            .name = canvas.layerNames[id],
            .opacity = layer.opacity,
            .contentGeneration = layer.contentGeneration,
            .tileCount = layer.tileCount,
            .descendants = layer.descendants,
            .blendMode = layer.blendMode,
            .flags = layer.flags & ~(LayerFlags::Deleted | LayerFlags::Dirty),
        };
        if (const int rc = put(file.layersDbi, Val(layerKey), Val(entry)); rc != MDBX_SUCCESS) {
            return fail("mdbx_put layer", rc);
        }
    }

    for (std::size_t i = 0; i < keys.size(); ++i) {
        const MDBX_val value{.iov_base = pixels.data() + (i * kTileBytes), .iov_len = kTileBytes};
        if (const int rc = put(file.tilesDbi, Val(keys[i]), value); rc != MDBX_SUCCESS) {
            return fail("mdbx_put tile", rc);
        }
    }

    // A failed commit ends the transaction too.
    if (const int rc = mdbx_txn_commit(txn); rc != MDBX_SUCCESS) {
        LogMdbx("mdbx_txn_commit", rc);
        return std::unexpected(Error::CanvasSaveFailed);
    }
    return {};
}

void CanvasMarkSaved(CanvasState& canvas) noexcept {
    for (TileState& tile : canvas.tiles) {
        tile.flags = (tile.flags | TileFlags::Saved) & ~TileFlags::Modified;
    }
    canvas.tileDeletes.clear();
    for (const Layer id : canvas.layersOrder) {
        canvas.layers[id].flags &= ~LayerFlags::Dirty;
    }
    for (Layer id = 0; id < canvas.largestLayer; ++id) {
        if (HasAnyFlag(canvas.layers[id].flags, LayerFlags::Deleted)) {
            canvas.layers[id] = {};
            canvas.layerNames[id] = {};
            canvas.freeLayers.insert(std::ranges::upper_bound(canvas.freeLayers, id, std::greater{}), id);
        }
    }
    canvas.modified = false;
}

[[nodiscard]] Result<Canvas> CanvasSlotTake() noexcept {
    const auto it = std::ranges::find(gAppState.canvases, kCanvasInvalid, &CanvasState::id);
    if (it == gAppState.canvases.end()) {
        MIDORI_LOG_ERROR("all {} canvases are in use", kCanvasMax);
        return std::unexpected(Error::CanvasLimitReached);
    }
    it->id = static_cast<Canvas>(it - gAppState.canvases.begin());
    it->cache = CacheOpen(it->id);
    return it->id;
}

[[nodiscard]] Result<void> TilesCacheOffscreen(CanvasState& canvas) noexcept {
    if (!canvas.cache.env) {
        return {};
    }
    std::vector<std::uint32_t> indices;
    std::vector<TileAtlasCell> cells;
    std::vector<TileKey> keys;
    for (std::uint32_t i = 0; i < canvas.tiles.size(); ++i) {
        const TileState& tile = canvas.tiles[i];
        if (HasAnyFlag(tile.flags, TileFlags::Loaded) && tile.visibleFrame != gAppState.frame
            && !TilePersisted(tile.flags) && !StrokeHasTile(gAppState.stroke, canvas.id, tile.key)) {
            indices.push_back(i);
            cells.push_back(tile.cell);
            keys.push_back(tile.key);
        }
    }
    if (indices.empty()) {
        return {};
    }
    std::vector<std::byte> pixels(cells.size() * kTileBytes);
    if (const auto res = GraphicsTilesDownload(cells, pixels); !res) {
        return res;
    }
    if (const auto res = TilesWrite(canvas.cache, keys, pixels); !res) {
        return res;
    }
    for (const std::uint32_t i : indices) {
        canvas.tiles[i].flags |= TileFlags::CachedDisk;
    }
    return {};
}

struct TileLoads {
    std::vector<std::uint32_t> indices;
    std::vector<TileAtlasCell> cells;
    std::vector<TileKey> keys;
};

// The cache holds the latest content of a tile edited since the save, so it goes first.
[[nodiscard]] Result<void> TilesLoadVisible(CanvasState& canvas) noexcept {
    TileLoads fromCache;
    TileLoads fromFile;
    for (std::uint32_t i = 0; i < canvas.tiles.size(); ++i) {
        TileState& tile = canvas.tiles[i];
        if (tile.visibleFrame != gAppState.frame || tile.cell != kTileAtlasCellInvalid) {
            continue;
        }
        MIDORI_ASSERT(TilePersisted(tile.flags), "tile {:#018x} evicted unsaved", tile.key);
        tile.cell = TileAtlasCellTake();
        if (tile.cell == kTileAtlasCellInvalid) {
            break;
        }
        TileLoads& loads = HasAnyFlag(tile.flags, TileFlags::CachedDisk) ? fromCache : fromFile;
        loads.indices.push_back(i);
        loads.cells.push_back(tile.cell);
        loads.keys.push_back(tile.key);
    }

    Result<void> result{};
    for (const auto& [loads, file] : {std::pair{&fromCache, &canvas.cache}, std::pair{&fromFile, &canvas.file}}) {
        if (loads->indices.empty()) {
            continue;
        }
        std::vector<std::byte> pixels(loads->keys.size() * kTileBytes);
        auto res = TilesRead(*file, loads->keys, pixels);
        if (res) {
            std::vector<const std::byte*> tilePixels(loads->keys.size());
            for (std::size_t i = 0; i < tilePixels.size(); ++i) {
                tilePixels[i] = pixels.data() + (i * kTileBytes);
            }
            res = GraphicsTilesUpload(loads->cells, tilePixels);
        }
        for (const std::uint32_t i : loads->indices) {
            TileState& tile = canvas.tiles[i];
            if (res) {
                tile.flags |= TileFlags::Loaded | TileFlags::Editable;
            } else {
                gAppState.freeAtlasCells.push_back(tile.cell);
                tile.cell = kTileAtlasCellInvalid;
            }
        }
        if (!res) {
            result = res;
        }
    }
    return result;
}

} // namespace

Result<Canvas> CanvasNew() noexcept {
    ZoneScopedN("CanvasNew");
    return CanvasSlotTake();
}

Result<Canvas> CanvasOpen(const std::filesystem::path& path) noexcept {
    ZoneScopedN("CanvasOpen");
    const auto opened = CanvasSlotTake();
    if (!opened) {
        return std::unexpected(opened.error());
    }
    CanvasState& canvas = gAppState.canvases[*opened];
    const auto file = CanvasFileOpen(path, false, Error::CanvasOpenFailed, MDBX_ENV_DEFAULTS);
    if (!file) {
        CanvasDestroy(*opened);
        return std::unexpected(file.error());
    }
    canvas.file = *file;

    MDBX_txn* txn = nullptr;
    if (const int rc = mdbx_txn_begin(canvas.file.env, nullptr, MDBX_TXN_RDONLY, &txn); rc != MDBX_SUCCESS) {
        LogMdbx("mdbx_txn_begin", rc);
        CanvasDestroy(*opened);
        return std::unexpected(Error::CanvasOpenFailed);
    }
    // MDBX_SUCCESS: the entry is there but holds no valid canvas data.
    const auto fail = [&path, txn, canvasId = *opened](const char* what, int rc,
                                                       Error error = Error::CanvasOpenFailed) {
        MIDORI_LOG_ERROR("CanvasOpen {}: {}: {}", PathUtf8(path), what,
                         rc == MDBX_SUCCESS ? "invalid data" : mdbx_strerror(rc));
        mdbx_txn_abort(txn);
        CanvasDestroy(canvasId);
        return std::unexpected(error);
    };

    const auto headerKey = std::to_underlying(CanvasFileMeta::Header);
    MDBX_val key = Val(headerKey);
    MDBX_val value{};
    if (const int rc = mdbx_get(txn, canvas.file.metaDbi, &key, &value); rc != MDBX_SUCCESS) {
        return fail("header", rc);
    }
    CanvasFileHeader header{};
    if (value.iov_len != sizeof(header)) {
        return fail("header", MDBX_SUCCESS);
    }
    std::memcpy(&header, value.iov_base, sizeof(header));
    if (header.magic != kCanvasFileMagic || header.version > kCanvasFileVersion
        || header.pixelFormat != PixelFormat::RGBA8 || header.largestLayer > kLayerMax) {
        return fail("header", MDBX_SUCCESS);
    }
    canvas.largestLayer = header.largestLayer;
    canvas.pixelFormat = header.pixelFormat;

    const auto orderKey = std::to_underlying(CanvasFileMeta::LayersOrder);
    key = Val(orderKey);
    if (const int rc = mdbx_get(txn, canvas.file.metaDbi, &key, &value); rc != MDBX_SUCCESS) {
        return fail("layers order", rc);
    }
    if (value.iov_len % sizeof(Layer) != 0) {
        return fail("layers order", MDBX_SUCCESS);
    }
    canvas.layersOrder.resize(value.iov_len / sizeof(Layer));
    if (value.iov_len != 0) {
        std::memcpy(canvas.layersOrder.data(), value.iov_base, value.iov_len);
    }
    std::vector<bool> used(canvas.largestLayer);
    for (const Layer id : canvas.layersOrder) {
        if (id >= canvas.largestLayer || used[id]) {
            return fail("layers order", MDBX_SUCCESS);
        }
        used[id] = true;
    }
    for (Layer id = canvas.largestLayer; id-- > 0;) {
        if (!used[id]) {
            canvas.freeLayers.push_back(id);
        }
    }

    MDBX_cursor* cursor = nullptr;
    if (const int rc = mdbx_cursor_open(txn, canvas.file.layersDbi, &cursor); rc != MDBX_SUCCESS) {
        return fail("layers", rc);
    }
    int rc = mdbx_cursor_get(cursor, &key, &value, MDBX_FIRST);
    for (; rc == MDBX_SUCCESS; rc = mdbx_cursor_get(cursor, &key, &value, MDBX_NEXT)) {
        std::uint32_t id{};
        std::memcpy(&id, key.iov_base, sizeof(id));
        if (key.iov_len != sizeof(id) || id >= canvas.largestLayer || !used[id]
            || value.iov_len != sizeof(LayerFileEntry)) {
            break;
        }
        LayerFileEntry entry{};
        std::memcpy(&entry, value.iov_base, sizeof(entry));
        // tileCount is recounted from the tiles below.
        canvas.layers[id] = LayerState{
            .opacity = entry.opacity,
            .contentGeneration = entry.contentGeneration,
            .tileCount = 0,
            .descendants = entry.descendants,
            .blendMode = entry.blendMode,
            .flags = entry.flags,
        };
        canvas.layerNames[id] = entry.name;
    }
    mdbx_cursor_close(cursor);
    if (rc != MDBX_NOTFOUND) {
        return fail("layers", rc);
    }

    if (rc = mdbx_cursor_open(txn, canvas.file.tilesDbi, &cursor); rc != MDBX_SUCCESS) {
        return fail("tiles", rc);
    }
    rc = mdbx_cursor_get(cursor, &key, &value, MDBX_FIRST);
    for (; rc == MDBX_SUCCESS; rc = mdbx_cursor_get(cursor, &key, &value, MDBX_NEXT)) {
        if (key.iov_len != sizeof(TileKey) || value.iov_len != kTileBytes) {
            break;
        }
        TileKey tileKey{};
        std::memcpy(&tileKey, key.iov_base, sizeof(tileKey));
        canvas.tileIndices.emplace(tileKey, static_cast<std::uint32_t>(canvas.tiles.size()));
        canvas.tiles.push_back(TileState{
            .key = tileKey,
            .edits = 0,
            .cacheEdits = 0,
            .saveEdits = 0,
            .visibleFrame = gAppState.frame,
            .cell = kTileAtlasCellInvalid,
            .flags = TileFlags::Saved,
            .transfer = TileTransfer::None,
        });
        ++canvas.layers[tileKey >> kTileLayerBitOffset].tileCount;
    }
    mdbx_cursor_close(cursor);
    if (rc != MDBX_NOTFOUND) {
        return fail("tiles", rc);
    }
    mdbx_txn_abort(txn);
    canvas.path = path;
    return *opened;
}

Result<void> CanvasSave(Canvas canvas) noexcept {
    ZoneScopedN("CanvasSave");
    CanvasState& state = gAppState.canvases[canvas];
    MIDORI_ASSERT(state.id == canvas && state.file.env, "canvas {} has no file", canvas);
    if (const auto res = CanvasFileWrite(state, state.file, false); !res) {
        return res;
    }
    CanvasMarkSaved(state);
    return {};
}

Result<Canvas> CanvasSaveAs(Canvas canvas, const std::filesystem::path& path) noexcept {
    ZoneScopedN("CanvasSaveAs");
    CanvasState& state = gAppState.canvases[canvas];
    MIDORI_ASSERT(state.id == canvas, "invalid canvas {}", canvas);
    std::error_code ec;
    if (state.file.env && std::filesystem::equivalent(path, state.path, ec)) {
        if (const auto res = CanvasSave(canvas); !res) {
            return std::unexpected(res.error());
        }
        return canvas;
    }

    // Written aside then renamed over `path`, so a failed save leaves the old file as it was.
    std::filesystem::path temp = path;
    temp += L".tmp";
    std::filesystem::remove(temp, ec);
    std::filesystem::remove(LockPath(temp), ec);
    auto file = CanvasFileOpen(temp, true, Error::CanvasSaveFailed, MDBX_ENV_DEFAULTS);
    if (!file) {
        return std::unexpected(file.error());
    }
    const auto written = CanvasFileWrite(state, *file, true);
    CanvasFileClose(*file);
    if (!written) {
        std::filesystem::remove(temp, ec);
        return std::unexpected(written.error());
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        MIDORI_LOG_ERROR("CanvasSaveAs: replacing {} failed: {}", PathUtf8(path), ec.message());
        std::filesystem::remove(temp, ec);
        return std::unexpected(Error::CanvasSaveFailed);
    }

    CanvasFileClose(state.file);
    state.path = path;
    CanvasMarkSaved(state);
    const auto reopened = CanvasFileOpen(path, false, Error::CanvasSaveFailed, MDBX_ENV_DEFAULTS);
    if (!reopened) {
        // Saved all the same; the next save goes through Save As again.
        MIDORI_LOG_WARN("CanvasSaveAs: {} saved but not reopened", PathUtf8(path));
        return canvas;
    }
    state.file = *reopened;
    return canvas;
}

void CanvasDestroy(Canvas canvas) noexcept {
    ZoneScopedN("CanvasDestroy");
    MIDORI_ASSERT(canvas < kCanvasMax && gAppState.canvases[canvas].id == canvas, "invalid canvas {}", canvas);
    CanvasState& state = gAppState.canvases[canvas];
    for (const TileState& tile : state.tiles) {
        if (tile.cell != kTileAtlasCellInvalid) {
            gAppState.freeAtlasCells.push_back(tile.cell);
        }
    }
    CanvasFileClose(state.file);
    CacheClose(state.cache);
    // In place: a CanvasState is too large for a temporary on the stack.
    std::destroy_at(&state);
    std::construct_at(&state);
}

Result<void> CanvasTilesStream() noexcept {
    ZoneScopedN("CanvasTilesStream");
    // Every canvas caches first: the atlas is shared, so any of them can take the cells it frees.
    Result<void> result{};
    for (CanvasState& canvas : gAppState.canvases) {
        if (canvas.id == kCanvasInvalid) {
            continue;
        }
        if (const auto res = TilesCacheOffscreen(canvas); !res) {
            result = res;
        }
    }
    for (CanvasState& canvas : gAppState.canvases) {
        if (canvas.id == kCanvasInvalid) {
            continue;
        }
        if (const auto res = TilesLoadVisible(canvas); !res) {
            result = res;
        }
    }
    return result;
}

} // namespace Midori
