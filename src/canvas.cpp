#include "midori/canvas.hpp"

#include "midori/app.hpp"
#include "midori/graphics.hpp"
#include "midori/job.hpp"
#include "midori/layer.hpp"
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
#include <qoi.h>
#include <tracy/Tracy.hpp>

namespace Midori {

// QOI's worst case for a tile: 5 bytes a texel, plus its header and end marker. Decoding needs less, 4 a texel.
constexpr std::size_t kQoiBytesMax = (std::size_t{kTileSize} * kTileSize * 5) + 14 + 8;

} // namespace Midori

// QOI_MALLOC in qoi.c. A thread encodes or decodes one tile at a time, so each thread reuses one buffer, made on its
// first call; QOI_FREE does nothing. A stream stays valid until the thread's next encode or decode.
extern "C" void* MidoriQoiBuffer(std::size_t size) noexcept {
    thread_local std::vector<std::byte> buffer(Midori::kQoiBytesMax);
    return size <= buffer.size() ? buffer.data() : nullptr;
}

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
    ZoneScopedN("CanvasFileClose");
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
    ZoneScopedN("CacheOpen");
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (!ec) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        std::array<char, 64> name{};
        std::format_to_n(name.data(), name.size() - 1, "midori-{}-{}.cache", stamp, canvas);
        const auto cache = CanvasFileOpen(dir / name.data(), true, Error::TileCacheFailed, MDBX_UTTERLY_NOSYNC);
        if (cache) {
            return *cache;
        }
    }
    MIDORI_LOG_WARN("canvas {}: no tile cache, unsaved tiles keep their atlas cells", canvas);
    return {};
}

void CacheClose(CanvasFile& cache) noexcept {
    ZoneScopedN("CacheClose");
    std::filesystem::path path;
    const wchar_t* raw = nullptr;
    if (cache.env && mdbx_env_get_pathW(cache.env, &raw) == MDBX_SUCCESS) {
        path = raw;
    }
    CanvasFileClose(cache);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// Every texel transparent: premultiplied, so every byte is zero. Only level-0 tiles are removed for it; an empty
// level 1-3 tile of a painted one would only be built again.
[[nodiscard]] bool TileEmpty(PixelFormat format, const std::byte* pixels) noexcept {
    const std::size_t pitch = TileLevelPitch(0, format);
    for (std::size_t row = 0; row < TileBytes(format); row += pitch) {
        std::uint64_t bits = 0;
        for (std::size_t i = row; i < row + pitch; i += sizeof(bits)) {
            std::uint64_t word = 0;
            std::memcpy(&word, pixels + i, sizeof(word));
            bits |= word;
        }
        if (bits != 0) {
            return false;
        }
    }
    return true;
}

// A tile of `level` laid out as TileLevelBytes in `pixels`. RGBA8 is a QOI stream in this thread's QOI buffer until its
// next encode or decode; QOI keeps the premultiplied channels as they are. Other formats have no codec yet and are
// `pixels` itself, raw. Null on failure.
[[nodiscard]] const void* TileEncode(TileMip level, PixelFormat format, const std::byte* pixels, int& size) noexcept {
    ZoneScopedN("TileEncode");
    if (format != PixelFormat::RGBA8) {
        size = static_cast<int>(TileLevelBytes(level, format));
        return pixels;
    }
    const std::uint32_t side = TileLevelSize(level);
    const std::size_t row = std::size_t{side} * 4llu;
    // Only rows narrower than the 256-byte pitch are padded: level 3, 32^2.
    std::array<std::byte, 32llu * 32llu * 4llu> packed;
    if (TileLevelPitch(level, format) != row) {
        MIDORI_ASSERT(row * side <= packed.size(), "level {} padded", level);
        for (std::uint32_t y = 0; y < side; ++y) {
            std::memcpy(packed.data() + (y * row), pixels + (y * TileLevelPitch(level, format)), row);
        }
        pixels = packed.data();
    }
    const qoi_desc desc{.width = side, .height = side, .channels = 4, .colorspace = QOI_LINEAR};
    return qoi_encode(pixels, &desc, &size);
}

// Into TileLevelBytes of `pixels`. Version 1 files hold RGBA8 tiles raw: QOI's magic "qoif" read as a premultiplied
// texel has more red than alpha, so no raw tile starts with it.
[[nodiscard]] bool TileDecode(TileMip level, PixelFormat format, const MDBX_val& value, std::byte* pixels) noexcept {
    ZoneScopedN("TileDecode");
    const auto* data = static_cast<const unsigned char*>(value.iov_base);
    if (format != PixelFormat::RGBA8) {
        if (value.iov_len != TileLevelBytes(level, format)) {
            return false;
        }
        std::memcpy(pixels, data, value.iov_len);
        return true;
    }
    if (value.iov_len == TileLevelBytes(level, format) && std::memcmp(data, "qoif", 4) != 0) {
        std::memcpy(pixels, data, value.iov_len);
        return true;
    }
    // The header's size is checked first, so a broken stream never asks for a large allocation.
    const std::uint32_t side = TileLevelSize(level);
    const auto bigEndian = [data](std::size_t offset) {
        return (std::uint32_t{data[offset]} << 24) | (std::uint32_t{data[offset + 1]} << 16)
             | (std::uint32_t{data[offset + 2]} << 8) | std::uint32_t{data[offset + 3]};
    };
    if (value.iov_len < 12 || bigEndian(4) != side || bigEndian(8) != side) {
        return false;
    }
    qoi_desc desc{};
    void* decoded = qoi_decode(data, static_cast<int>(value.iov_len), &desc, 4);
    if (!decoded) {
        return false;
    }
    const std::size_t row = std::size_t{side} * 4;
    for (std::uint32_t y = 0; y < side; ++y) {
        std::memcpy(pixels + (y * TileLevelPitch(level, format)), static_cast<const std::byte*>(decoded) + (y * row),
                    row);
    }
    return true;
}

// A TileBytes stride per key in `pixels`, each tile TileLevelBytes of its level.
[[nodiscard]] Result<void> TilesRead(const CanvasFile& file, PixelFormat format, std::span<const TileKey> keys,
                                     std::span<std::byte> pixels) noexcept {
    ZoneScopedN("TilesRead");
    MIDORI_ASSERT(pixels.size() == keys.size() * TileBytes(format), "{} bytes for {} tiles", pixels.size(),
                  keys.size());
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
        if (rc != MDBX_SUCCESS
            || !TileDecode(TileKeyMip(keys[i]), format, value, pixels.data() + (i * TileBytes(format)))) {
            MIDORI_LOG_ERROR("reading tile {:#018x}: {}", keys[i],
                             rc == MDBX_SUCCESS ? "invalid data" : mdbx_strerror(rc));
            mdbx_txn_abort(txn);
            return std::unexpected(Error::TileReadFailed);
        }
    }
    mdbx_txn_abort(txn);
    return {};
}

[[nodiscard]] Result<void> TilesWrite(const CanvasFile& file, PixelFormat format, std::span<const TileKey> keys,
                                      std::span<const std::byte> pixels) noexcept {
    ZoneScopedN("TilesWrite");
    MIDORI_ASSERT(pixels.size() == keys.size() * TileBytes(format), "{} bytes for {} tiles", pixels.size(),
                  keys.size());
    MDBX_txn* txn = nullptr;
    if (const int rc = mdbx_txn_begin(file.env, nullptr, MDBX_TXN_READWRITE, &txn); rc != MDBX_SUCCESS) {
        LogMdbx("mdbx_txn_begin", rc);
        return std::unexpected(Error::TileCacheFailed);
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        int size = 0;
        const void* encoded = TileEncode(TileKeyMip(keys[i]), format, pixels.data() + (i * TileBytes(format)), size);
        if (!encoded) {
            MIDORI_LOG_ERROR("encoding tile {:#018x} failed", keys[i]);
            mdbx_txn_abort(txn);
            return std::unexpected(Error::TileCacheFailed);
        }
        MDBX_val key = Val(keys[i]);
        MDBX_val value{.iov_base = const_cast<void*>(encoded), .iov_len = static_cast<std::size_t>(size)};
        const int rc = mdbx_put(txn, file.tilesDbi, &key, &value, MDBX_UPSERT);
        if (rc != MDBX_SUCCESS) {
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
// first, so a tile deleted then created again is written. Transparent level-0 tiles are not written but deleted from
// the file with their level 1-3 tiles, and returned in `empties`, sorted, for the caller to delete once committed.
[[nodiscard]] Result<void> CanvasFileWrite(CanvasState& canvas, const CanvasFile& file, bool all,
                                           std::vector<TileKey>& empties) noexcept {
    ZoneScopedN("CanvasFileWrite");
    // Tiles in the atlas are downloaded. Evicted ones are read from the cache, or from the canvas's own file when
    // unchanged since its last save.
    std::vector<TileAtlasCell> cells;
    std::vector<TileKey> keys; // the atlas ones, then the cached ones, then the saved ones
    std::vector<TileKey> cachedKeys;
    std::vector<TileKey> savedKeys;
    for (const TileState& tile : canvas.tiles) {
        // The selection is not saved.
        if ((!all && !HasAnyFlag(tile.flags, TileFlags::Modified)) || HasAnyFlag(tile.flags, TileFlags::Composite)
            || HasAnyFlag(canvas.layers[TileKeyLayer(tile.key)].flags, LayerFlags::Mask)) {
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
    const std::size_t tileBytes = TileBytes(canvas.pixelFormat);
    std::vector<std::byte> pixels((keys.size() + cachedKeys.size() + savedKeys.size()) * tileBytes);
    const std::span<std::byte> downloaded = std::span(pixels).first(cells.size() * tileBytes);
    const std::span<std::byte> cached = std::span(pixels).subspan(downloaded.size(), cachedKeys.size() * tileBytes);
    const std::span<std::byte> saved = std::span(pixels).subspan(downloaded.size() + cached.size());
    if (const auto res = GraphicsTilesDownload(cells, downloaded); !res) {
        return std::unexpected(res.error());
    }
    if (!TilesRead(canvas.cache, canvas.pixelFormat, cachedKeys, cached)
        || !TilesRead(canvas.file, canvas.pixelFormat, savedKeys, saved)) {
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
            if (!HasAnyFlag(canvas.layers[id].flags, LayerFlags::Purged)) {
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

    // The layers in the order, and the Deleted ones history can still bring back, with their tiles.
    for (Layer id = 0; id < canvas.largestLayer; ++id) {
        const LayerState& layer = canvas.layers[id];
        const bool kept =
            canvas.layerHeights[id] != kLayerInvalid
            || (HasAnyFlag(layer.flags, LayerFlags::Deleted) && !HasAnyFlag(layer.flags, LayerFlags::Purged));
        if (!kept || (!all && !HasAnyFlag(layer.flags, LayerFlags::Dirty))) {
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
            .flags = layer.flags & ~LayerFlags::Dirty,
        };
        if (const int rc = put(file.layersDbi, Val(layerKey), Val(entry)); rc != MDBX_SUCCESS) {
            return fail("mdbx_put layer", rc);
        }
    }

    // A tile in the stroke is written whatever it holds: the stroke keeps painting into its cell.
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (TileKeyMip(keys[i]) == 0 && !StrokeHasTile(gAppState.stroke, canvas.id, keys[i])
            && TileEmpty(canvas.pixelFormat, pixels.data() + (i * tileBytes))) {
            empties.push_back(keys[i]);
        }
    }
    std::ranges::sort(empties);
    for (const TileKey empty : all ? std::span<const TileKey>{} : std::span<const TileKey>(empties)) {
        for (TileMip mip = 0; mip < kTileMipMax; ++mip) {
            MDBX_val key = Val(TileKeyWithMip(empty, mip));
            if (const int rc = mdbx_del(txn, file.tilesDbi, &key, nullptr); rc != MDBX_SUCCESS && rc != MDBX_NOTFOUND) {
                return fail("mdbx_del", rc);
            }
        }
    }

    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (std::ranges::binary_search(empties, TileKeyWithMip(keys[i], 0))) {
            continue;
        }
        int size = 0;
        const void* encoded =
            TileEncode(TileKeyMip(keys[i]), canvas.pixelFormat, pixels.data() + (i * tileBytes), size);
        if (!encoded) {
            MIDORI_LOG_ERROR("encoding tile {:#018x} failed", keys[i]);
            mdbx_txn_abort(txn);
            return std::unexpected(Error::CanvasSaveFailed);
        }
        const MDBX_val value{.iov_base = const_cast<void*>(encoded), .iov_len = static_cast<std::size_t>(size)};
        const int rc = put(file.tilesDbi, Val(keys[i]), value);
        if (rc != MDBX_SUCCESS) {
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

// After the save committed, so the file no longer holds them either.
void CanvasEmptiesDelete(CanvasState& canvas, std::span<const TileKey> empties) noexcept {
    for (const TileKey key : empties) {
        const Tile tile{
            .canvas = canvas.id,
            .mip = 0,
            .layer = static_cast<Layer>(key >> kTileLayerBitOffset),
            .gridPos = TileKeyGridPos(key),
        };
        if (!TileDelete(tile)) {
            MIDORI_LOG_WARN("empty tile {:#018x} not deleted", key);
        }
    }
}

void CanvasMarkSaved(CanvasState& canvas) noexcept {
    ZoneScopedN("CanvasMarkSaved");
    for (TileState& tile : canvas.tiles) {
        if (!HasAnyFlag(tile.flags, TileFlags::Composite)
            && !HasAnyFlag(canvas.layers[TileKeyLayer(tile.key)].flags, LayerFlags::Mask)) {
            tile.flags = (tile.flags | TileFlags::Saved) & ~TileFlags::Modified;
        }
    }
    canvas.tileDeletes.clear();
    for (Layer id = 0; id < canvas.largestLayer; ++id) {
        canvas.layers[id].flags &= ~LayerFlags::Dirty;
        if (HasAnyFlag(canvas.layers[id].flags, LayerFlags::Purged)) {
            canvas.layers[id] = {};
            canvas.layerNames[id] = {};
            canvas.freeLayers.insert(std::ranges::upper_bound(canvas.freeLayers, id, std::greater{}), id);
        }
    }
    canvas.modified = false;
}

[[nodiscard]] Result<Canvas> CanvasSlotTake() noexcept {
    ZoneScopedN("CanvasSlotTake");
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
// Grid positions whose composites one update queues, for the same reason.
constexpr std::uint32_t kCompositesPerUpdate = 64;
// Grid positions of a merge one update queues, and how many ahead of them stream their sources in.
constexpr std::uint32_t kMergesPerUpdate = 16;
constexpr std::size_t kMergeWindow = 32;

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
    ZoneScopedN("TileJobsFinish");
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
                tile->transfer &= ~TileTransfer::Cache;
                // Any edit since the download bumped `edits`, so the tile is still what was found empty.
                const bool current = !job.failed && tile->edits == tile->cacheEdits;
                if (current && job.empty && !StrokeHasTile(gAppState.stroke, slot.canvas, slot.key)) {
                    const Tile empty{
                        .canvas = slot.canvas,
                        .mip = 0,
                        .layer = static_cast<Layer>(slot.key >> kTileLayerBitOffset),
                        .gridPos = TileKeyGridPos(slot.key),
                    };
                    if (!TileDelete(empty)) {
                        MIDORI_LOG_WARN("empty tile {:#018x} not deleted", slot.key);
                    }
                } else if (current && !job.empty) {
                    tile->flags |= TileFlags::CachedDisk;
                }
            }
            slot.state = TileSlotState::Free;
        }
    }
}

// Uploads are done once the GPU passes their fence; downloads then go to the writer.
void TileSlotsAdvance() noexcept {
    ZoneScopedN("TileSlotsAdvance");
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
            .format = gAppState.canvases[slot.canvas].pixelFormat,
            .failed = false,
            .empty = false,
        });
        slot.state = TileSlotState::Job;
    }
}

// Read uploads take their cell now, not before the read, so a slow read holds no atlas space. The tile counts
// as Loaded from here: the next submit records its copy before any draw or stroke.
void TileUploadsPlace() noexcept {
    ZoneScopedN("TileUploadsPlace");
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
    ZoneScopedN("TileCachesSchedule");
    if (!canvas.cache.env) {
        return;
    }
    auto& slots = gAppState.transfers.downloads;
    for (TileState& tile : canvas.tiles) {
        if (!HasAnyFlag(tile.flags, TileFlags::Loaded) || tile.visibleFrame == gAppState.frame
            || TilePersisted(tile.flags) || HasAnyFlag(tile.flags, TileFlags::Composite)
            || tile.transfer != TileTransfer::None || StrokeHasTile(gAppState.stroke, canvas.id, tile.key)) {
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

// The cache holds the latest content of a tile edited since the save, so it goes first. Composite tiles are composited
// instead.
void TileLoadsSchedule(CanvasState& canvas) noexcept {
    ZoneScopedN("TileLoadsSchedule");
    auto& slots = gAppState.transfers.uploads;
    for (TileState& tile : canvas.tiles) {
        if (tile.visibleFrame != gAppState.frame || tile.cell != kTileAtlasCellInvalid
            || tile.transfer != TileTransfer::None || HasAnyFlag(tile.flags, TileFlags::Composite)) {
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
            .format = canvas.pixelFormat,
            .failed = false,
            .empty = false,
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

// Loaded, current level-0 root tiles get their missing level 1-3 tiles, built by the next submit, which views zoomed
// out draw instead of level 0. Composites like their source, an evicted one is built again. Up to `budget` level-0
// tiles per update, shared by every canvas.
void TileMipsSchedule(CanvasState& canvas, std::uint32_t& budget) noexcept {
    ZoneScopedN("TileMipsSchedule");
    const Layer root = canvas.layersOrder.back();
    for (std::uint32_t i = 0; i < canvas.tiles.size() && budget > 0; ++i) {
        // Copied: TileInsert grows `tiles`.
        const TileKey key = canvas.tiles[i].key;
        if (TileKeyMip(key) != 0 || TileKeyLayer(key) != root || !HasAnyFlag(canvas.tiles[i].flags, TileFlags::Loaded)
            || HasAnyFlag(canvas.tiles[i].flags, TileFlags::Dirty) || std::ranges::contains(canvas.mipBuilds, key)) {
            continue;
        }
        // Missing, evicted, or inserted by a queue the submit dropped.
        bool missing = false;
        for (TileMip mip = 1; mip < kTileMipMax; ++mip) {
            const auto found = canvas.tileIndices.find(TileKeyWithMip(key, mip));
            missing = missing || found == canvas.tileIndices.end()
                   || !HasAnyFlag(canvas.tiles[found->second].flags, TileFlags::Loaded);
        }
        if (!missing) {
            continue;
        }
        // Queued first, so taking the cells cannot evict the source.
        canvas.mipBuilds.push_back(key);
        --budget;
        for (TileMip mip = 1; mip < kTileMipMax; ++mip) {
            const TileKey mipKey = TileKeyWithMip(key, mip);
            const auto found = canvas.tileIndices.find(mipKey);
            if (found != canvas.tileIndices.end() && canvas.tiles[found->second].cell != kTileAtlasCellInvalid) {
                continue;
            }
            const TileAtlasCell cell = TileAtlasCellTake(mip, canvas.id, mipKey);
            if (cell == kTileAtlasCellInvalid) {
                return;
            }
            if (found != canvas.tileIndices.end()) {
                canvas.tiles[found->second].cell = cell;
                continue;
            }
            TileInsert(canvas, mipKey, cell).flags = TileFlags::Composite;
        }
    }
}

// The composite tile `key`, with a cell, made if missing; null when no cell can be taken. Kept from eviction until the
// submit records it.
[[nodiscard]] TileState* CompositeTileTake(CanvasState& canvas, TileKey key) noexcept {
    auto found = canvas.tileIndices.find(key);
    if (found == canvas.tileIndices.end()) {
        const TileAtlasCell cell = TileAtlasCellTake(0, canvas.id, key);
        if (cell == kTileAtlasCellInvalid) {
            return nullptr;
        }
        TileState& tile = TileInsert(canvas, key, cell);
        tile.flags = TileFlags::Composite | TileFlags::Dirty;
        return &tile;
    }
    TileState& tile = canvas.tiles[found->second];
    tile.visibleFrame = gAppState.frame;
    if (tile.cell == kTileAtlasCellInvalid) {
        tile.cell = TileAtlasCellTake(0, canvas.id, key);
        if (tile.cell == kTileAtlasCellInvalid) {
            return nullptr;
        }
        tile.flags |= TileFlags::Dirty;
    }
    return &tile;
}

// Readies `folder`'s composite at `gridPos` (level-0 key, layer bits zero): wants the tiles of its shown children
// there, readies its children folders not current, and takes its tile, queued in `queue` after theirs. False when
// nothing it shows has a tile there: its tile is deleted then. `ready` turns false while a source still streams in or
// a cell cannot be taken.
bool CompositePrepare(CanvasState& canvas, Layer folder, TileKey gridPos, bool& ready,
                      std::vector<TileKey>& queue) noexcept {
    const Layer top = canvas.layerHeights[folder];
    const auto bottom = static_cast<int>(top) - canvas.layers[folder].descendants;
    bool content = false;
    // Its children, top first: past each one, the layers inside it.
    for (int h = static_cast<int>(top) - 1; h >= bottom;
         h -= canvas.layers[canvas.layersOrder[static_cast<std::size_t>(h)]].descendants + 1) {
        const Layer child = canvas.layersOrder[static_cast<std::size_t>(h)];
        const LayerState& layer = canvas.layers[child];
        if (HasAnyFlag(layer.flags, LayerFlags::Hidden) || layer.opacity <= 0.0f) {
            continue;
        }
        const auto found = canvas.tileIndices.find(TileGridPosLayerKey(gridPos, child));
        if (HasAnyFlag(layer.flags, LayerFlags::Folder)) {
            if (found != canvas.tileIndices.end()) {
                TileState& tile = canvas.tiles[found->second];
                if (HasAnyFlag(tile.flags, TileFlags::Loaded) && !HasAnyFlag(tile.flags, TileFlags::Dirty)) {
                    tile.visibleFrame = gAppState.frame;
                    content = true;
                    continue;
                }
            }
            content = CompositePrepare(canvas, child, gridPos, ready, queue) || content;
            continue;
        }
        if (found == canvas.tileIndices.end()) {
            continue;
        }
        TileState& tile = canvas.tiles[found->second];
        tile.visibleFrame = gAppState.frame;
        ready = ready && HasAnyFlag(tile.flags, TileFlags::Loaded) && !HasAnyFlag(tile.transfer, TileTransfer::Load);
        content = true;
    }
    const TileKey key = TileGridPosLayerKey(gridPos, folder);
    if (!content) {
        const Tile tile{.canvas = canvas.id, .mip = 0, .layer = folder, .gridPos = TileKeyGridPos(gridPos)};
        if (!TileDelete(tile)) {
            MIDORI_LOG_WARN("composite tile {:#018x} not deleted", key);
        }
        return false;
    }
    if (!CompositeTileTake(canvas, key)) {
        ready = false;
        return true;
    }
    queue.push_back(key);
    return true;
}

// The root composites of the grid positions shown whose content changed, or that a view needs while they have no cell:
// at level 0, or at a level whose tile is not loaded. The stroke's positions go first, so painting shows every frame.
// Up to `budget` positions per update, shared by every canvas.
void TileCompositesSchedule(CanvasState& canvas, std::uint32_t& budget) noexcept {
    ZoneScopedN("TileCompositesSchedule");
    const Layer root = canvas.layersOrder.back();
    const StrokeState& stroke = gAppState.stroke;
    std::ranges::stable_partition(canvas.compositeWants, [&](const CompositeWant& want) {
        return stroke.canvas == canvas.id && std::ranges::any_of(stroke.tiles, [&](const StrokeTile& tile) {
                   return TileGridPosKey(tile.key) == want.gridPos;
               });
    });
    std::vector<TileKey> queue;
    for (const CompositeWant& want : canvas.compositeWants) {
        if (budget == 0) {
            return;
        }
        const TileKey key = TileGridPosLayerKey(want.gridPos, root);
        if (std::ranges::contains(canvas.composites, key)) {
            continue;
        }
        const auto found = canvas.tileIndices.find(key);
        if (found != canvas.tileIndices.end()) {
            const TileState& tile = canvas.tiles[found->second];
            const auto level = canvas.tileIndices.find(TileKeyWithMip(key, want.level));
            const bool levelShown = want.level != 0 && level != canvas.tileIndices.end()
                                 && HasAnyFlag(canvas.tiles[level->second].flags, TileFlags::Loaded);
            if (!HasAnyFlag(tile.flags, TileFlags::Dirty) && (tile.cell != kTileAtlasCellInvalid || levelShown)) {
                continue;
            }
        }
        bool ready = true;
        queue.clear();
        // Every position tried counts, ready or not: the tiles the tries want must fit the atlas. Zoomed far out, the
        // positions shown would otherwise want more level-0 tiles than it holds, keep each other's from loading and
        // never finish.
        --budget;
        // A position whose layers are all hidden lost its tile and queues nothing.
        CompositePrepare(canvas, root, want.gridPos, ready, queue);
        if (ready && !queue.empty()) {
            canvas.composites.insert(canvas.composites.end(), queue.begin(), queue.end());
        }
    }
}

// The grid positions of the canvas's merge whose sources are loaded get their result tile and go to the next submit,
// up to `budget` per update; the next ones up to kMergeWindow want their sources meanwhile. A folder flattened is
// composited there first. Positions where nothing shows are dropped. Once every result tile is composited, the result
// takes the sources' place.
void TileMergesSchedule(CanvasState& canvas, std::uint32_t& budget) noexcept {
    LayerMergeState& merge = canvas.merge;
    if (merge.result == kLayerInvalid) {
        return;
    }
    ZoneScopedN("TileMergesSchedule");
    if (merge.remaining.empty() && merge.queued.empty()) {
        LayerMergeFinish(canvas);
        return;
    }
    std::vector<TileKey> queue;
    std::size_t looked = 0;
    for (auto it = merge.remaining.begin(); it != merge.remaining.end() && looked < kMergeWindow && budget > 0;) {
        ++looked;
        const TileKey gridPos = *it;
        bool ready = true;
        if (merge.lower == kLayerInvalid) {
            const auto found = canvas.tileIndices.find(TileGridPosLayerKey(gridPos, merge.upper));
            if (found == canvas.tileIndices.end() || !HasAnyFlag(canvas.tiles[found->second].flags, TileFlags::Loaded)
                || HasAnyFlag(canvas.tiles[found->second].flags, TileFlags::Dirty)) {
                queue.clear();
                if (!CompositePrepare(canvas, merge.upper, gridPos, ready, queue)) {
                    it = merge.remaining.erase(it);
                    continue;
                }
                for (const TileKey key : queue) {
                    if (ready && !std::ranges::contains(canvas.composites, key)) {
                        canvas.composites.push_back(key);
                    }
                }
                ++it;
                continue;
            }
            canvas.tiles[found->second].visibleFrame = gAppState.frame;
        } else {
            bool any = false;
            for (const Layer layer : {merge.lower, merge.upper}) {
                const auto found = canvas.tileIndices.find(TileGridPosLayerKey(gridPos, layer));
                if (found == canvas.tileIndices.end()) {
                    continue;
                }
                TileState& tile = canvas.tiles[found->second];
                tile.visibleFrame = gAppState.frame;
                ready = ready && HasAnyFlag(tile.flags, TileFlags::Loaded)
                     && !HasAnyFlag(tile.transfer, TileTransfer::Load);
                any = true;
            }
            if (!any) {
                it = merge.remaining.erase(it);
                continue;
            }
        }
        if (!ready) {
            ++it;
            continue;
        }
        // Left by a submit that found a source gone, it keeps its cell.
        const TileKey key = TileGridPosLayerKey(gridPos, merge.result);
        if (!canvas.tileIndices.contains(key)) {
            const TileAtlasCell cell = TileAtlasCellTake(0, canvas.id, key);
            if (cell == kTileAtlasCellInvalid) {
                return;
            }
            TileInsert(canvas, key, cell);
        }
        merge.queued.push_back(gridPos);
        it = merge.remaining.erase(it);
        --budget;
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
    const auto canvas = CanvasSlotTake();
    if (canvas) {
        LayersInit(gAppState.canvases[*canvas]);
    }
    return canvas;
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
    std::vector<bool> inOrder(canvas.largestLayer);
    for (const Layer id : canvas.layersOrder) {
        if (id >= canvas.largestLayer || inOrder[id]) {
            return fail("layers order", MDBX_SUCCESS);
        }
        inOrder[id] = true;
    }

    // Out of the order, only Deleted layers have an entry; with no history left to bring them back, they are purged.
    std::vector<bool> stored(canvas.largestLayer);
    MDBX_cursor* cursor = nullptr;
    if (const int rc = mdbx_cursor_open(txn, canvas.file.layersDbi, &cursor); rc != MDBX_SUCCESS) {
        return fail("layers", rc);
    }
    int rc = mdbx_cursor_get(cursor, &key, &value, MDBX_FIRST);
    for (; rc == MDBX_SUCCESS; rc = mdbx_cursor_get(cursor, &key, &value, MDBX_NEXT)) {
        std::uint32_t id{};
        std::memcpy(&id, key.iov_base, sizeof(id));
        if (key.iov_len != sizeof(id) || id >= canvas.largestLayer || value.iov_len != sizeof(LayerFileEntry)) {
            break;
        }
        LayerFileEntry entry{};
        std::memcpy(&entry, value.iov_base, sizeof(entry));
        if (inOrder[id] == HasAnyFlag(entry.flags, LayerFlags::Deleted)) {
            break;
        }
        // tileCount is recounted from the tiles below.
        canvas.layers[id] = LayerState{
            .opacity = entry.opacity,
            .contentGeneration = entry.contentGeneration,
            .tileCount = 0,
            .descendants = entry.descendants,
            .blendMode = entry.blendMode,
            .flags = inOrder[id] ? entry.flags : entry.flags | LayerFlags::Purged,
        };
        canvas.layerNames[id] = entry.name;
        canvas.layerNames[id].back() = '\0';
        stored[id] = true;
    }
    mdbx_cursor_close(cursor);
    if (rc != MDBX_NOTFOUND) {
        return fail("layers", rc);
    }
    for (const Layer id : canvas.layersOrder) {
        if (!stored[id]) {
            return fail("layers", MDBX_SUCCESS);
        }
    }

    // Version 2 has no root folder; canvases made new never had a layer, their tiles are layer 0's.
    if (header.version < 3 && canvas.layersOrder.empty()) {
        if (canvas.largestLayer != 0) {
            return fail("layers order", MDBX_SUCCESS);
        }
        LayersInit(canvas);
        inOrder.assign(canvas.largestLayer, true);
        stored.assign(canvas.largestLayer, true);
    } else if (header.version < 3) {
        if (canvas.largestLayer == kLayerMax) {
            return fail("layers order", MDBX_SUCCESS);
        }
        const Layer root = canvas.largestLayer++;
        canvas.layers[root] = LayerState{
            .descendants = static_cast<Layer>(canvas.layersOrder.size()),
            .flags = LayerFlags::Folder | LayerFlags::Internal | LayerFlags::Dirty,
        };
        canvas.layerNames[root] = LayerName("Canvas");
        canvas.layersOrder.push_back(root);
        inOrder.push_back(true);
        stored.push_back(true);
    }
    // The root tops the order around everything else; every folder sits inside the one around it.
    if (canvas.layersOrder.empty()) {
        return fail("layers order", MDBX_SUCCESS);
    }
    std::vector<std::size_t> bottoms; // of the folders around the height, innermost last
    for (std::size_t h = canvas.layersOrder.size(); h-- > 0;) {
        const LayerState& layer = canvas.layers[canvas.layersOrder[h]];
        const bool top = h == canvas.layersOrder.size() - 1;
        if (top != HasAnyFlag(layer.flags, LayerFlags::Internal)
            || (top && (!HasAnyFlag(layer.flags, LayerFlags::Folder) || layer.descendants != h))) {
            return fail("layers order", MDBX_SUCCESS);
        }
        while (!bottoms.empty() && bottoms.back() > h) {
            bottoms.pop_back();
        }
        if (!HasAnyFlag(layer.flags, LayerFlags::Folder)) {
            continue;
        }
        if (layer.descendants > h || (!bottoms.empty() && h - layer.descendants < bottoms.back())) {
            return fail("layers order", MDBX_SUCCESS);
        }
        bottoms.push_back(h - layer.descendants);
    }
    // Purged ids stay taken until the next save deletes them.
    for (Layer id = canvas.largestLayer; id-- > 0;) {
        if (!inOrder[id] && !stored[id]) {
            canvas.freeLayers.push_back(id);
        }
    }
    LayersIndex(canvas);
    canvas.activeLayer =
        canvas.layersOrder.size() > 1 ? canvas.layersOrder[canvas.layersOrder.size() - 2] : kLayerInvalid;

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
        // Decoding checks the content when the tile is read.
        if (value.iov_len == 0) {
            break;
        }
        // A purged layer's tiles stay in the file until the next save deletes its range.
        if (TileKeyLayer(tileKey) >= canvas.largestLayer || !inOrder[TileKeyLayer(tileKey)]) {
            continue;
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
        ++canvas.layers[TileKeyLayer(tileKey)].tileCount;
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
    // The result's tiles would be written without its layer.
    if (state.merge.result != kLayerInvalid) {
        return std::unexpected(Error::CanvasBusy);
    }
    std::vector<TileKey> empties;
    if (const auto res = CanvasFileWrite(state, state.file, false, empties); !res) {
        return res;
    }
    CanvasEmptiesDelete(state, empties);
    CanvasMarkSaved(state);
    return {};
}

Result<Canvas> CanvasSaveAs(Canvas canvas, const std::filesystem::path& path) noexcept {
    ZoneScopedN("CanvasSaveAs");
    CanvasState& state = gAppState.canvases[canvas];
    MIDORI_ASSERT(state.id == canvas, "invalid canvas {}", canvas);
    if (state.merge.result != kLayerInvalid) {
        return std::unexpected(Error::CanvasBusy);
    }
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
    std::vector<TileKey> empties;
    const auto written = CanvasFileWrite(state, *file, true, empties);
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
    CanvasEmptiesDelete(state, empties);
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

bool CanvasTilesStream() noexcept {
    ZoneScopedN("CanvasTilesStream");
    TileJobsFinish();
    TileSlotsAdvance();
    TileUploadsPlace();
    std::uint32_t mipBudget = kMipBuildsPerUpdate;
    std::uint32_t compositeBudget = kCompositesPerUpdate;
    std::uint32_t mergeBudget = kMergesPerUpdate;
    for (CanvasState& canvas : gAppState.canvases) {
        if (canvas.id != kCanvasInvalid) {
            // First: they want the layer tiles they need, which the loads then stream in.
            TileCompositesSchedule(canvas, compositeBudget);
            TileMergesSchedule(canvas, mergeBudget);
            TileCachesSchedule(canvas);
            TileLoadsSchedule(canvas);
            TileMipsSchedule(canvas, mipBudget);
        }
    }
    TileAtlasRelieve();
    return mipBudget == 0 || compositeBudget == 0 || mergeBudget == 0;
}

Result<void> CanvasTilesGet(const CanvasState& canvas, std::span<const TileKey> keys,
                            std::span<std::byte> pixels) noexcept {
    ZoneScopedN("CanvasTilesGet");
    const std::size_t tileBytes = TileBytes(canvas.pixelFormat);
    MIDORI_ASSERT(pixels.size() == keys.size() * tileBytes, "{} bytes for {} tiles", pixels.size(), keys.size());
    std::ranges::fill(pixels, std::byte{0});
    // Each source gathers its tiles back to back, then they go where `keys` has them.
    std::vector<TileAtlasCell> cells;
    std::vector<std::size_t> downloaded;
    std::vector<TileKey> cachedKeys;
    std::vector<std::size_t> cached;
    std::vector<TileKey> savedKeys;
    std::vector<std::size_t> saved;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto found = canvas.tileIndices.find(keys[i]);
        if (found == canvas.tileIndices.end()) {
            continue;
        }
        const TileState& tile = canvas.tiles[found->second];
        if (tile.cell != kTileAtlasCellInvalid) {
            cells.push_back(tile.cell);
            downloaded.push_back(i);
        } else if (HasAnyFlag(tile.flags, TileFlags::CachedDisk)) {
            cachedKeys.push_back(tile.key);
            cached.push_back(i);
        } else if (HasAnyFlag(tile.flags, TileFlags::Saved)) {
            savedKeys.push_back(tile.key);
            saved.push_back(i);
        }
    }
    std::vector<std::byte> gathered;
    const auto scatter = [&](std::span<const std::size_t> at) {
        for (std::size_t j = 0; j < at.size(); ++j) {
            std::memcpy(pixels.data() + (at[j] * tileBytes), gathered.data() + (j * tileBytes), tileBytes);
        }
    };
    // Only these wait for the GPU.
    if (!cells.empty()) {
        gathered.resize(cells.size() * tileBytes);
        if (const auto res = GraphicsTilesDownload(cells, gathered); !res) {
            return std::unexpected(res.error());
        }
        scatter(downloaded);
    }
    gathered.resize(cachedKeys.size() * tileBytes);
    if (const auto res = TilesRead(canvas.cache, canvas.pixelFormat, cachedKeys, gathered); !res) {
        return std::unexpected(res.error());
    }
    scatter(cached);
    gathered.resize(savedKeys.size() * tileBytes);
    if (const auto res = TilesRead(canvas.file, canvas.pixelFormat, savedKeys, gathered); !res) {
        return std::unexpected(res.error());
    }
    scatter(saved);
    return {};
}

CanvasFileSize CanvasFileSizeGet(const CanvasFile& file) noexcept {
    MDBX_envinfo info{};
    if (!file.env || mdbx_env_info_ex(file.env, nullptr, &info, sizeof(info)) != MDBX_SUCCESS) {
        return {};
    }
    return CanvasFileSize{
        .file = info.mi_geo.current,
        .used = (info.mi_last_pgno + 1) * std::uint64_t{info.mi_dxb_pagesize},
    };
}

Result<void> CanvasJobExecute(Job& job) noexcept {
    ZoneScopedN("CanvasJobExecute");
    const std::span<const TileKey> key(&job.key, 1);
    const std::span<std::byte> pixels(job.pixels, TileBytes(job.format));
    if (job.type == JobType::TileRead) {
        return TilesRead(job.file, job.format, key, pixels);
    }
    job.empty = TileKeyMip(job.key) == 0 && TileEmpty(job.format, job.pixels);
    return job.empty ? Result<void>{} : TilesWrite(job.file, job.format, key, pixels);
}

} // namespace Midori
