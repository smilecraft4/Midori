#include "midori/canvas.hpp"

#include "midori/app.hpp"
#include "midori/graphics.hpp"
#include "midori/job.hpp"
#include "midori/log.hpp"
#include "midori/tile.hpp"

#include <algorithm>
#include <array>
#include <bit>
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

// A kTileBytes stride per key in `pixels`, each tile TileLevelBytes of its level.
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
        if (rc != MDBX_SUCCESS || value.iov_len != TileLevelBytes(TileKeyMip(keys[i]))) {
            MIDORI_LOG_ERROR("reading tile {:#018x}: {}", keys[i],
                             rc == MDBX_SUCCESS ? "invalid size" : mdbx_strerror(rc));
            mdbx_txn_abort(txn);
            return std::unexpected(Error::TileReadFailed);
        }
        std::memcpy(pixels.data() + (i * kTileBytes), value.iov_base, value.iov_len);
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
        MDBX_val value{.iov_base = const_cast<std::byte*>(pixels.data() + (i * kTileBytes)),
                       .iov_len = TileLevelBytes(TileKeyMip(keys[i]))};
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
        const MDBX_val value{.iov_base = pixels.data() + (i * kTileBytes),
                             .iov_len = TileLevelBytes(TileKeyMip(keys[i]))};
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

// Above kAtlasEvictHigh of every block the atlases can hold, the tiles shown the longest ago are evicted down to
// kAtlasEvictLow, so a load rarely waits for a cell.
constexpr std::uint32_t kAtlasBlockMax = std::uint32_t{kTileAtlasPageMax} * kTileAtlasBlocks;
constexpr std::uint32_t kAtlasEvictHigh = kAtlasBlockMax / 8 * 7;
constexpr std::uint32_t kAtlasEvictLow = kAtlasBlockMax / 4 * 3;
// Level-0 tiles whose level 1-3 tiles one update queues, to keep each submit's dispatches small.
constexpr std::uint32_t kMipBuildsPerUpdate = 16;

// Null once the tile was deleted, its slot orphaned or its canvas destroyed.
[[nodiscard]] TileState* TileFind(Canvas canvas, TileKey key) noexcept {
    if (canvas == kCanvasInvalid || gAppState.canvases[canvas].id != canvas) {
        return nullptr;
    }
    CanvasState& state = gAppState.canvases[canvas];
    const auto found = state.tileIndices.find(key);
    return found == state.tileIndices.end() ? nullptr : &state.tiles[found->second];
}

[[nodiscard]] TileTransferSlot TileSlotFind(const std::array<TileSlot, kTileTransferSlots>& slots,
                                            TileSlotState state) noexcept {
    const auto found = std::ranges::find(slots, state, &TileSlot::state);
    return static_cast<TileTransferSlot>(found - slots.begin());
}

void TileJobsFinish() noexcept {
    TileTransfersState& transfers = gAppState.transfers;
    std::vector<Job> jobs;
    JobsTakeFinished(jobs);
    for (const Job& job : jobs) {
        if (job.type == JobType::TileRead) {
            TileSlot& slot = transfers.uploads[job.slot];
            TileState* tile = TileFind(slot.canvas, slot.key);
            if (tile && !job.failed) {
                slot.state = TileSlotState::Ready;
                continue;
            }
            if (tile) {
                tile->transfer &= ~TileTransfer::Load;
            }
            slot.state = TileSlotState::Free;
        } else {
            TileSlot& slot = transfers.downloads[job.slot];
            if (TileState* tile = TileFind(slot.canvas, slot.key)) {
                if (!job.failed && tile->edits == tile->cacheEdits) {
                    tile->flags |= TileFlags::CachedDisk;
                }
                tile->transfer &= ~TileTransfer::Cache;
            }
            slot.state = TileSlotState::Free;
        }
    }
}

// Uploads are done once the GPU passes their fence; downloads then go to the writer.
void TileSlotsAdvance() noexcept {
    TileTransfersState& transfers = gAppState.transfers;
    const std::uint64_t completed = GraphicsCompletedFence();
    for (TileSlot& slot : transfers.uploads) {
        if (slot.state == TileSlotState::Gpu && slot.fence <= completed) {
            if (TileState* tile = TileFind(slot.canvas, slot.key)) {
                tile->transfer &= ~TileTransfer::Load;
            }
            slot.state = TileSlotState::Free;
        }
    }
    for (TileTransferSlot i = 0; i < kTileTransferSlots; ++i) {
        TileSlot& slot = transfers.downloads[i];
        if (slot.state != TileSlotState::Gpu || slot.fence > completed) {
            continue;
        }
        if (!TileFind(slot.canvas, slot.key)) {
            slot.state = TileSlotState::Free;
            continue;
        }
        JobSubmit(Job{
            .file = gAppState.canvases[slot.canvas].cache,
            .key = slot.key,
            .pixels = GraphicsTileDownloadMemory(i),
            .slot = i,
            .canvas = slot.canvas,
            .type = JobType::TileWrite,
            .failed = false,
        });
        slot.state = TileSlotState::Job;
    }
}

// Read uploads take their cell now, not before the read, so a slow read holds no atlas space. The tile counts
// as Loaded from here: the next submit records its copy before any draw or stroke.
void TileUploadsPlace() noexcept {
    for (TileSlot& slot : gAppState.transfers.uploads) {
        if (slot.state != TileSlotState::Ready) {
            continue;
        }
        TileState* tile = TileFind(slot.canvas, slot.key);
        if (!tile) {
            slot.state = TileSlotState::Free;
            continue;
        }
        const TileAtlasCell cell = TileAtlasCellTake(TileKeyMip(slot.key), slot.canvas, slot.key);
        if (cell == kTileAtlasCellInvalid) {
            return;
        }
        tile->cell = cell;
        tile->flags |= TileFlags::Loaded | TileFlags::Editable;
        slot.cell = cell;
        slot.state = TileSlotState::Record;
    }
}

// Loaded tiles no longer shown whose content is stored nowhere go to the cache, which makes them evictable.
void TileCachesSchedule(CanvasState& canvas) noexcept {
    if (!canvas.cache.env) {
        return;
    }
    auto& slots = gAppState.transfers.downloads;
    for (TileState& tile : canvas.tiles) {
        if (!HasAnyFlag(tile.flags, TileFlags::Loaded) || tile.visibleFrame == gAppState.frame
            || TilePersisted(tile.flags) || tile.transfer != TileTransfer::None
            || StrokeHasTile(gAppState.stroke, canvas.id, tile.key)) {
            continue;
        }
        const TileTransferSlot i = TileSlotFind(slots, TileSlotState::Free);
        if (i == kTileTransferSlots) {
            return;
        }
        slots[i] = TileSlot{
            .fence = 0,
            .key = tile.key,
            .cell = tile.cell,
            .canvas = canvas.id,
            .state = TileSlotState::Record,
        };
        tile.transfer |= TileTransfer::Cache;
        tile.cacheEdits = tile.edits;
    }
}

// The cache holds the latest content of a tile edited since the save, so it goes first.
void TileLoadsSchedule(CanvasState& canvas) noexcept {
    auto& slots = gAppState.transfers.uploads;
    for (TileState& tile : canvas.tiles) {
        if (tile.visibleFrame != gAppState.frame || tile.cell != kTileAtlasCellInvalid
            || tile.transfer != TileTransfer::None) {
            continue;
        }
        MIDORI_ASSERT(TilePersisted(tile.flags), "tile {:#018x} evicted unsaved", tile.key);
        const TileTransferSlot i = TileSlotFind(slots, TileSlotState::Free);
        if (i == kTileTransferSlots) {
            return;
        }
        JobSubmit(Job{
            .file = HasAnyFlag(tile.flags, TileFlags::CachedDisk) ? canvas.cache : canvas.file,
            .key = tile.key,
            .pixels = GraphicsTileUploadMemory(i),
            .slot = i,
            .canvas = canvas.id,
            .type = JobType::TileRead,
            .failed = false,
        });
        slots[i] = TileSlot{
            .fence = 0,
            .key = tile.key,
            .cell = kTileAtlasCellInvalid,
            .canvas = canvas.id,
            .state = TileSlotState::Job,
        };
        tile.transfer |= TileTransfer::Load;
    }
}

// Loaded level-0 tiles outside the stroke get their missing level 1-3 tiles, built by the next submit. They start
// Modified, so streaming caches them once off screen and a save writes them, while views zoomed out draw them
// instead of level 0. Up to `budget` level-0 tiles per update, shared by every canvas.
void TileMipsSchedule(CanvasState& canvas, std::uint32_t& budget) noexcept {
    for (std::uint32_t i = 0; i < canvas.tiles.size() && budget > 0; ++i) {
        // Copied: TileInsert grows `tiles`.
        const TileKey key = canvas.tiles[i].key;
        if (TileKeyMip(key) != 0 || !HasAnyFlag(canvas.tiles[i].flags, TileFlags::Loaded)
            || StrokeHasTile(gAppState.stroke, canvas.id, key) || std::ranges::contains(canvas.mipBuilds, key)) {
            continue;
        }
        // Missing, or inserted by a queue the submit dropped.
        bool missing = false;
        for (TileMip mip = 1; mip < kTileMipMax; ++mip) {
            const auto found = canvas.tileIndices.find(TileKeyWithMip(key, mip));
            missing = missing || found == canvas.tileIndices.end()
                   || (!HasAnyFlag(canvas.tiles[found->second].flags, TileFlags::Loaded)
                       && canvas.tiles[found->second].cell != kTileAtlasCellInvalid);
        }
        if (!missing) {
            continue;
        }
        // Queued first, so taking the cells cannot evict the source.
        canvas.mipBuilds.push_back(key);
        --budget;
        for (TileMip mip = 1; mip < kTileMipMax; ++mip) {
            const TileKey mipKey = TileKeyWithMip(key, mip);
            if (canvas.tileIndices.contains(mipKey)) {
                continue;
            }
            const TileAtlasCell cell = TileAtlasCellTake(mip, canvas.id, mipKey);
            if (cell == kTileAtlasCellInvalid) {
                return;
            }
            TileInsert(canvas, mipKey, cell);
        }
    }
}

void TileAtlasRelieve() noexcept {
    const TileAtlasState& atlas = gAppState.atlas;
    std::uint32_t used = 0;
    for (std::uint32_t page = 0; page < std::uint32_t{atlas.count} * kTileAtlasPages; ++page) {
        used += static_cast<std::uint32_t>(std::popcount(atlas.used[page]));
    }
    if (used <= kAtlasEvictHigh) {
        return;
    }
    ZoneScopedN("TileAtlasRelieve");
    struct Candidate {
        std::uint32_t visibleFrame;
        std::uint32_t index;
        Canvas canvas;
    };
    std::vector<Candidate> candidates;
    for (const CanvasState& canvas : gAppState.canvases) {
        if (canvas.id == kCanvasInvalid) {
            continue;
        }
        for (std::uint32_t i = 0; i < canvas.tiles.size(); ++i) {
            if (TileEvictable(canvas.id, canvas.tiles[i])) {
                candidates.push_back({.visibleFrame = canvas.tiles[i].visibleFrame, .index = i, .canvas = canvas.id});
            }
        }
    }
    std::ranges::sort(candidates, {}, &Candidate::visibleFrame);
    for (const Candidate& candidate : candidates) {
        if (used <= kAtlasEvictLow) {
            break;
        }
        TileState& tile = gAppState.canvases[candidate.canvas].tiles[candidate.index];
        used -= TileAtlasLevelBlocks(TileAtlasCellLevel(tile.cell));
        TileEvict(tile);
    }
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
        if (key.iov_len != sizeof(TileKey)) {
            break;
        }
        TileKey tileKey{};
        std::memcpy(&tileKey, key.iov_base, sizeof(tileKey));
        if (value.iov_len != TileLevelBytes(TileKeyMip(tileKey))) {
            break;
        }
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

    // Readers may still be loading tiles from the old file.
    JobsWaitIdle();
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
    // Its jobs use its envs; after them, its slots either have no copy recorded yet, or finish on the GPU orphaned.
    JobsWaitIdle();
    TileJobsFinish();
    for (auto* slots : {&gAppState.transfers.uploads, &gAppState.transfers.downloads}) {
        for (TileSlot& slot : *slots) {
            if (slot.canvas == canvas) {
                slot.state = slot.state == TileSlotState::Gpu ? TileSlotState::Gpu : TileSlotState::Free;
                slot.canvas = kCanvasInvalid;
            }
        }
    }
    for (const TileState& tile : state.tiles) {
        if (tile.cell != kTileAtlasCellInvalid) {
            TileAtlasCellFree(tile.cell);
        }
    }
    for (const HistoryTile& tile : state.history.tiles) {
        if (tile.cell != kTileAtlasCellInvalid) {
            TileAtlasCellFree(tile.cell);
        }
    }
    // Ops still waiting on its cells write them before anything that takes them again, see HistoryPoolState.
    for (const HistoryBlock& block : state.history.blocks) {
        gAppState.historyPool.freeSlots.push_back(block.slot);
    }
    CanvasFileClose(state.file);
    CacheClose(state.cache);
    // In place: a CanvasState is too large for a temporary on the stack.
    std::destroy_at(&state);
    std::construct_at(&state);
}

void CanvasTilesStream() noexcept {
    ZoneScopedN("CanvasTilesStream");
    TileJobsFinish();
    TileSlotsAdvance();
    TileUploadsPlace();
    std::uint32_t mipBudget = kMipBuildsPerUpdate;
    for (CanvasState& canvas : gAppState.canvases) {
        if (canvas.id != kCanvasInvalid) {
            TileCachesSchedule(canvas);
            TileLoadsSchedule(canvas);
            TileMipsSchedule(canvas, mipBudget);
        }
    }
    TileAtlasRelieve();
}

Result<void> CanvasJobExecute(const Job& job) noexcept {
    const std::span<const TileKey> key(&job.key, 1);
    const std::span<std::byte> pixels(job.pixels, kTileBytes);
    return job.type == JobType::TileRead ? TilesRead(job.file, key, pixels) : TilesWrite(job.file, key, pixels);
}

} // namespace Midori
