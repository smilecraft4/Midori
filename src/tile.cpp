#include "midori/tile.hpp"

#include "midori/app.hpp"
#include "midori/graphics.hpp"
#include "midori/layer.hpp"
#include "midori/log.hpp"

#include <bit>
#include <limits>
#include <optional>
#include <vector>

#include <tracy/Tracy.hpp>

namespace Midori {

namespace {

// Bit i set: the aligned run of `level` starting at Morton block i is free.
[[nodiscard]] std::uint64_t FreeRuns(std::uint64_t used, std::uint8_t level) noexcept {
    const std::uint64_t free1 = ~used;
    if (level == 3) {
        return free1;
    }
    const std::uint64_t pairs = free1 & (free1 >> 1);
    const std::uint64_t free4 = pairs & (pairs >> 2) & 0x1111111111111111;
    if (level == 2) {
        return free4;
    }
    const std::uint64_t run8 = free4 & (free4 >> 4);
    const std::uint64_t free16 = run8 & (run8 >> 8) & 0x0001000100010001;
    if (level == 1) {
        return free16;
    }
    return used == 0 ? 1 : 0;
}

[[nodiscard]] std::uint64_t BlockMask(std::uint8_t block, std::uint8_t level) noexcept {
    const std::uint8_t blocks = TileAtlasLevelBlocks(level);
    return blocks == kTileAtlasBlocks ? ~std::uint64_t{0} : ((std::uint64_t{1} << blocks) - 1) << block;
}

[[nodiscard]] std::uint32_t TileAtlasPageEnd() noexcept {
    return std::uint32_t{gAppState.atlas.count} * kTileAtlasPages;
}

[[nodiscard]] TileAtlasCell TileAtlasCellPlace(TileAtlasPage page, std::uint8_t block, std::uint8_t level,
                                               TileAtlasOwner owner) noexcept {
    TileAtlasState& atlas = gAppState.atlas;
    atlas.used[page] |= BlockMask(block, level);
    owner.level = level;
    for (std::uint8_t b = block; b < block + TileAtlasLevelBlocks(level); ++b) {
        atlas.owners[page][b] = owner;
    }
    return TileAtlasCellMake(page, block, level);
}

// Best fit: the fullest page with a free run of the size.
[[nodiscard]] TileAtlasCell TileAtlasCellPlaceFree(std::uint8_t level, TileAtlasOwner owner) noexcept {
    ZoneScopedN("TileAtlasCellPlaceFree");
    const TileAtlasState& atlas = gAppState.atlas;
    std::uint32_t best = kTileAtlasPageMax;
    int bestUsed = -1;
    for (std::uint32_t page = 0; page < TileAtlasPageEnd(); ++page) {
        if (FreeRuns(atlas.used[page], level) != 0 && std::popcount(atlas.used[page]) > bestUsed) {
            best = page;
            bestUsed = std::popcount(atlas.used[page]);
        }
    }
    if (best == kTileAtlasPageMax) {
        return kTileAtlasCellInvalid;
    }
    const auto block = static_cast<std::uint8_t>(std::countr_zero(FreeRuns(atlas.used[best], level)));
    return TileAtlasCellPlace(static_cast<TileAtlasPage>(best), block, level, owner);
}

// The owner's tile when it is evictable, null otherwise.
[[nodiscard]] TileState* TileOwnerEvictable(const TileAtlasOwner& owner) noexcept {
    if (owner.canvas == kCanvasInvalid) {
        return nullptr;
    }
    CanvasState& canvas = gAppState.canvases[owner.canvas];
    const auto found = canvas.tileIndices.find(owner.key);
    if (found == canvas.tileIndices.end()) {
        return nullptr;
    }
    TileState& tile = canvas.tiles[found->second];
    return TileEvictable(owner.canvas, tile) ? &tile : nullptr;
}

// The newest `visibleFrame` among the owners of an aligned run, or nothing when one of them cannot be evicted.
// Steps over each owner's cell; one larger than the run covers all of it.
[[nodiscard]] std::optional<std::uint32_t> TileAtlasRunAge(TileAtlasPage page, std::uint8_t origin,
                                                           std::uint8_t level) noexcept {
    const TileAtlasState& atlas = gAppState.atlas;
    std::uint32_t newest = 0;
    const unsigned end = unsigned{origin} + TileAtlasLevelBlocks(level);
    for (unsigned b = origin; b < end;) {
        if (((atlas.used[page] >> b) & 1) == 0) {
            ++b;
            continue;
        }
        const TileAtlasOwner& owner = atlas.owners[page][b];
        const TileState* tile = TileOwnerEvictable(owner);
        if (!tile) {
            return std::nullopt;
        }
        newest = std::max(newest, tile->visibleFrame);
        b += TileAtlasLevelBlocks(owner.level);
    }
    return newest;
}

// Evicts every owner of the aligned run whose owners were shown the longest ago, then places the cell there.
// The GPU may still read an evicted cell in a frame in flight; its next fill is recorded after that frame.
[[nodiscard]] TileAtlasCell TileAtlasCellPlaceEvicting(std::uint8_t level, TileAtlasOwner owner) noexcept {
    ZoneScopedN("TileAtlasCellPlaceEvicting");
    TileAtlasState& atlas = gAppState.atlas;
    const std::uint8_t blocks = TileAtlasLevelBlocks(level);
    std::uint32_t bestPage = kTileAtlasPageMax;
    std::uint8_t bestOrigin = 0;
    std::uint32_t bestAge = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t page = 0; page < TileAtlasPageEnd(); ++page) {
        for (unsigned origin = 0; origin < kTileAtlasBlocks; origin += blocks) {
            const auto age =
                TileAtlasRunAge(static_cast<TileAtlasPage>(page), static_cast<std::uint8_t>(origin), level);
            if (age && *age < bestAge) {
                bestPage = page;
                bestOrigin = static_cast<std::uint8_t>(origin);
                bestAge = *age;
            }
        }
    }
    if (bestPage == kTileAtlasPageMax) {
        return kTileAtlasCellInvalid;
    }
    const auto page = static_cast<TileAtlasPage>(bestPage);
    for (unsigned b = bestOrigin; b < unsigned{bestOrigin} + blocks;) {
        if (((atlas.used[page] >> b) & 1) == 0) {
            ++b;
            continue;
        }
        TileState* tile = TileOwnerEvictable(atlas.owners[page][b]);
        MIDORI_ASSERT(tile, "atlas page {} block {} became unevictable", page, b);
        b += TileAtlasLevelBlocks(atlas.owners[page][b].level);
        TileEvict(*tile);
    }
    return TileAtlasCellPlace(page, bestOrigin, level, owner);
}

} // namespace

// A level-0 tile queued to build its level 1-3 tiles, or to swap blocks with history, stays until the submit records
// it. A composite tile is composited again when needed.
bool TileEvictable(Canvas canvas, const TileState& tile) noexcept {
    return HasAnyFlag(tile.flags, TileFlags::Loaded) && tile.visibleFrame != gAppState.frame
        && (TilePersisted(tile.flags) || HasAnyFlag(tile.flags, TileFlags::Composite))
        && tile.transfer == TileTransfer::None && !StrokeHasTile(gAppState.stroke, canvas, tile.key)
        && std::ranges::find(gAppState.canvases[canvas].mipBuilds, tile.key)
               == gAppState.canvases[canvas].mipBuilds.end()
        && !std::ranges::binary_search(gAppState.historyPool.ops, tile.cell, {}, &HistoryOp::cell);
}

void TileEvict(TileState& tile) noexcept {
    ZoneScopedN("TileEvict");
    TileAtlasCellFree(tile.cell);
    tile.cell = kTileAtlasCellInvalid;
    tile.flags &= ~(TileFlags::Loaded | TileFlags::Editable);
}

TileAtlasCell TileAtlasCellTake(std::uint8_t level, Canvas canvas, TileKey key) noexcept {
    ZoneScopedN("TileAtlasCellTake");
    MIDORI_ASSERT(level < kTileMipMax, "atlas level {}", level);
    const TileAtlasOwner owner{.key = key, .canvas = canvas, .level = level};
    if (const TileAtlasCell cell = TileAtlasCellPlaceFree(level, owner); cell != kTileAtlasCellInvalid) {
        return cell;
    }
    // A new atlas before any eviction: evicted tiles cost a transfer to show again.
    TileAtlasState& atlas = gAppState.atlas;
    if (atlas.count < kTileAtlasMax && GraphicsTileAtlasCreate(atlas.count)) {
        ++atlas.count;
        return TileAtlasCellPlaceFree(level, owner);
    }
    return TileAtlasCellPlaceEvicting(level, owner);
}

void TileAtlasCellFree(TileAtlasCell cell) noexcept {
    ZoneScopedN("TileAtlasCellFree");
    TileAtlasState& atlas = gAppState.atlas;
    const TileAtlasPage page = TileAtlasCellPage(cell);
    const std::uint8_t block = TileAtlasCellBlock(cell);
    const std::uint8_t level = TileAtlasCellLevel(cell);
    MIDORI_ASSERT((atlas.used[page] & BlockMask(block, level)) == BlockMask(block, level), "atlas cell {:#x} not taken",
                  cell);
    atlas.used[page] &= ~BlockMask(block, level);
    for (std::uint8_t b = block; b < block + TileAtlasLevelBlocks(level); ++b) {
        atlas.owners[page][b] = {};
    }
}

void TileAtlasCellOwn(TileAtlasCell cell, Canvas canvas, TileKey key) noexcept {
    ZoneScopedN("TileAtlasCellOwn");
    const TileAtlasPage page = TileAtlasCellPage(cell);
    const std::uint8_t block = TileAtlasCellBlock(cell);
    const std::uint8_t level = TileAtlasCellLevel(cell);
    for (std::uint8_t b = block; b < block + TileAtlasLevelBlocks(level); ++b) {
        gAppState.atlas.owners[page][b] = TileAtlasOwner{.key = key, .canvas = canvas, .level = level};
    }
}

Result<void> TileNew(Tile tile) noexcept {
    ZoneScopedN("TileNew");
    MIDORI_ASSERT(tile.canvas < kCanvasMax && gAppState.canvases[tile.canvas].id == tile.canvas, "invalid canvas {}",
                  tile.canvas);
    CanvasState& canvas = gAppState.canvases[tile.canvas];
    const TileKey key = TileKeyMake(tile.layer, tile.mip, tile.gridPos);
    if (canvas.tileIndices.contains(key)) {
        return {};
    }
    const TileAtlasCell cell = TileAtlasCellTake(tile.mip, tile.canvas, key);
    if (cell == kTileAtlasCellInvalid) {
        MIDORI_LOG_WARN("TileNew: all {} atlas pages hold visible or unsaved tiles", kTileAtlasPageMax);
        return std::unexpected(Error::TileAtlasFull);
    }
    TileInsert(canvas, key, cell);
    canvas.creates.push_back(key);
    canvas.modified = true;
    if (tile.mip == 0) {
        LayerCompositesTouch(canvas, key);
    }
    return {};
}

TileState& TileInsert(CanvasState& canvas, TileKey key, TileAtlasCell cell) noexcept {
    ZoneScopedN("TileInsert");
    canvas.tileIndices.emplace(key, static_cast<std::uint32_t>(canvas.tiles.size()));
    canvas.tiles.push_back(TileState{
        .key = key,
        .edits = 0,
        .cacheEdits = 0,
        .saveEdits = 0,
        .visibleFrame = gAppState.frame,
        .cell = cell,
        .flags = TileFlags::Modified,
        .transfer = TileTransfer::None,
    });
    ++canvas.layers[key >> kTileLayerBitOffset].tileCount;
    return canvas.tiles.back();
}

void TileMipsDelete(Canvas canvas, TileKey key) noexcept {
    ZoneScopedN("TileMipsDelete");
    const auto layer = static_cast<Layer>(key >> kTileLayerBitOffset);
    for (TileMip mip = 1; mip < kTileMipMax; ++mip) {
        if (!TileDelete(Tile{.canvas = canvas, .mip = mip, .layer = layer, .gridPos = TileKeyGridPos(key)})) {
            MIDORI_LOG_WARN("TileMipsDelete: tile {:#018x} level {} not deleted", key, mip);
        }
    }
}

TileMip TileDrawnLevel(const CanvasState& canvas, TileKey key, TileMip viewLevel) noexcept {
    const auto loaded = [&](int mip) {
        if (mip < 0 || mip >= kTileMipMax) {
            return false;
        }
        const auto found = canvas.tileIndices.find(TileKeyWithMip(key, static_cast<TileMip>(mip)));
        return found != canvas.tileIndices.end() && HasAnyFlag(canvas.tiles[found->second].flags, TileFlags::Loaded);
    };
    for (int distance = 0; distance < kTileMipMax; ++distance) {
        if (loaded(viewLevel - distance)) {
            return static_cast<TileMip>(viewLevel - distance);
        }
        if (loaded(viewLevel + distance)) {
            return static_cast<TileMip>(viewLevel + distance);
        }
    }
    return kTileMipMax;
}

bool TileDrawn(const CanvasState& canvas, const TileState& tile, TileMip viewLevel) noexcept {
    return HasAnyFlag(tile.flags, TileFlags::Loaded)
        && TileKeyMip(tile.key) == TileDrawnLevel(canvas, tile.key, viewLevel);
}

Result<void> TileDelete(Tile tile) noexcept {
    ZoneScopedN("TileDelete");
    MIDORI_ASSERT(tile.canvas < kCanvasMax && gAppState.canvases[tile.canvas].id == tile.canvas, "invalid canvas {}",
                  tile.canvas);
    CanvasState& canvas = gAppState.canvases[tile.canvas];
    const TileKey key = TileKeyMake(tile.layer, tile.mip, tile.gridPos);
    if (!canvas.tileIndices.contains(key)) {
        return {};
    }
    if (tile.mip == 0) {
        TileMipsDelete(tile.canvas, key);
        std::erase(canvas.mipBuilds, key);
        LayerCompositesTouch(canvas, key);
    }
    const auto it = canvas.tileIndices.find(key);
    const std::uint32_t index = it->second;
    canvas.tileIndices.erase(it);
    // The GPU may still read the cell in a frame in flight; its next fill is recorded after that frame.
    if (canvas.tiles[index].cell != kTileAtlasCellInvalid) {
        TileAtlasCellFree(canvas.tiles[index].cell);
    }
    // Copies not recorded yet would touch the freed cell. Slots a job or the GPU holds are orphaned: they finish
    // and are dropped, even if a tile with the same key comes back meanwhile.
    for (auto* slots : {&gAppState.transfers.uploads, &gAppState.transfers.downloads}) {
        for (TileSlot& slot : *slots) {
            if (slot.state == TileSlotState::Free || slot.canvas != tile.canvas || slot.key != key) {
                continue;
            }
            if (slot.state == TileSlotState::Ready || slot.state == TileSlotState::Record) {
                slot.state = TileSlotState::Free;
            }
            slot.canvas = kCanvasInvalid;
        }
    }
    std::erase(canvas.creates, key);
    if (HasAnyFlag(canvas.tiles[index].flags, TileFlags::Saved)) {
        canvas.tileDeletes.push_back(key);
    }
    if (index != canvas.tiles.size() - 1) {
        canvas.tiles[index] = canvas.tiles.back();
        canvas.tileIndices[canvas.tiles[index].key] = index;
    }
    canvas.tiles.pop_back();
    --canvas.layers[tile.layer].tileCount;
    canvas.modified = true;
    return {};
}

} // namespace Midori
