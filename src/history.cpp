#include "midori/history.hpp"

#include "midori/app.hpp"
#include "midori/log.hpp"
#include "midori/stroke.hpp"
#include "midori/tile.hpp"

#include <algorithm>
#include <bit>
#include <span>
#include <utility>

#include <tracy/Tracy.hpp>

namespace Midori {

namespace {

// How many updates a step waits for its tiles to stream in before the pending steps are dropped.
constexpr std::uint32_t kHistoryWaitUpdates = 300;

// Frees the slots and cells of records [first, end), leaving the records in place.
void HistoryRecordsFree(HistoryState& history, std::size_t first, std::size_t end) noexcept {
    ZoneScopedN("HistoryRecordsFree");
    for (std::size_t r = first; r < end; ++r) {
        const HistoryRecord& record = history.records[r];
        for (std::uint32_t i = record.firstBlock; i < record.firstBlock + record.blockCount; ++i) {
            gAppState.historyPool.freeSlots.push_back(history.blocks[i].slot);
        }
        for (std::uint32_t i = record.firstTile; i < record.firstTile + record.tileCount; ++i) {
            if (history.tiles[i].cell != kTileAtlasCellInvalid) {
                TileAtlasCellFree(history.tiles[i].cell);
            }
        }
    }
}

// Drops records [current, end).
void HistoryRedoDrop(HistoryState& history) noexcept {
    ZoneScopedN("HistoryRedoDrop");
    if (history.current == history.records.size()) {
        return;
    }
    HistoryRecordsFree(history, history.current, history.records.size());
    const HistoryRecord& first = history.records[history.current];
    history.blocks.resize(first.firstBlock);
    history.tiles.resize(first.firstTile);
    history.records.resize(history.current);
    history.pending = std::min(history.pending, 0);
}

// Drops the oldest record of any canvas: when its canvas has nothing left to undo, every record of it, since each
// one only redoes onto the one before. False when no canvas has a record.
[[nodiscard]] bool HistoryDropOldest() noexcept {
    ZoneScopedN("HistoryDropOldest");
    HistoryState* oldest = nullptr;
    for (CanvasState& canvas : gAppState.canvases) {
        HistoryState& history = canvas.history;
        if (canvas.id != kCanvasInvalid && !history.records.empty()
            && (!oldest || history.records.front().sequence < oldest->records.front().sequence)) {
            oldest = &history;
        }
    }
    if (!oldest) {
        return false;
    }
    HistoryState& history = *oldest;
    const std::size_t count = history.current == 0 ? history.records.size() : 1;
    HistoryRecordsFree(history, 0, count);
    const bool all = count == history.records.size();
    const std::uint32_t dropBlocks =
        all ? static_cast<std::uint32_t>(history.blocks.size()) : history.records[count].firstBlock;
    const std::uint32_t dropTiles =
        all ? static_cast<std::uint32_t>(history.tiles.size()) : history.records[count].firstTile;
    history.blocks.erase(history.blocks.begin(), history.blocks.begin() + dropBlocks);
    history.tiles.erase(history.tiles.begin(), history.tiles.begin() + dropTiles);
    history.records.erase(history.records.begin(), history.records.begin() + static_cast<std::ptrdiff_t>(count));
    for (HistoryRecord& record : history.records) {
        record.firstBlock -= dropBlocks;
        record.firstTile -= dropTiles;
    }
    history.current -= std::min(history.current, static_cast<std::uint32_t>(count));
    history.pending = std::clamp(history.pending, -static_cast<std::int32_t>(history.current),
                                 static_cast<std::int32_t>(history.records.size() - history.current));
    return true;
}

} // namespace

void HistoryPushStroke(StrokeState& stroke) noexcept {
    ZoneScopedN("HistoryPushStroke");
    MIDORI_ASSERT(gAppState.historyPool.ops.empty(), "history pushed with ops waiting");
    HistoryPoolState& pool = gAppState.historyPool;
    HistoryState& history = gAppState.canvases[stroke.canvas].history;
    HistoryRedoDrop(history);

    std::uint32_t needed = 0;
    for (const StrokeTile& tile : stroke.tiles) {
        if (tile.backupCell != kTileAtlasCellInvalid && tile.backupCell != kTileAtlasCellClear) {
            needed += static_cast<std::uint32_t>(std::popcount(tile.touched));
        }
    }
    // Undoing past an edit that is not recorded would put back stale blocks around it.
    if (needed > kHistorySlotsMax) {
        MIDORI_LOG_WARN("history: stroke touches {} blocks, more than the store holds; history cleared", needed);
        HistoryRecordsFree(history, 0, history.records.size());
        history = {};
        return;
    }
    while (pool.freeSlots.size() < needed && HistoryDropOldest()) {}

    HistoryRecord record{
        .sequence = pool.sequence++,
        .firstBlock = static_cast<std::uint32_t>(history.blocks.size()),
        .blockCount = 0,
        .firstTile = static_cast<std::uint32_t>(history.tiles.size()),
        .tileCount = 0,
    };
    for (const StrokeTile& tile : stroke.tiles) {
        if (tile.backupCell == kTileAtlasCellClear) {
            history.tiles.push_back(HistoryTile{.key = tile.key, .cell = kTileAtlasCellInvalid});
            continue;
        }
        if (tile.backupCell == kTileAtlasCellInvalid) {
            continue;
        }
        for (std::uint64_t touched = tile.touched; touched != 0; touched &= touched - 1) {
            const auto block = static_cast<std::uint8_t>(std::countr_zero(touched));
            const std::uint32_t slot = pool.freeSlots.back();
            pool.freeSlots.pop_back();
            history.blocks.push_back(HistoryBlock{.key = tile.key, .slot = slot, .block = block});
            pool.ops.push_back(HistoryOp{.slot = slot, .cell = tile.backupCell, .block = block, .swap = false});
        }
    }
    record.blockCount = static_cast<std::uint32_t>(history.blocks.size()) - record.firstBlock;
    record.tileCount = static_cast<std::uint32_t>(history.tiles.size()) - record.firstTile;
    if (record.blockCount == 0 && record.tileCount == 0) {
        return;
    }
    history.records.push_back(record);
    ++history.current;
}

void HistoryUpdate(Canvas canvas) noexcept {
    CanvasState& state = gAppState.canvases[canvas];
    HistoryState& history = state.history;
    HistoryPoolState& pool = gAppState.historyPool;
    if (history.pending == 0 || gAppState.stroke.canvas == canvas || !pool.ops.empty()) {
        return;
    }
    ZoneScopedN("HistoryUpdate");
    const bool undo = history.pending < 0;
    const HistoryRecord record = history.records[undo ? history.current - 1 : history.current];
    const std::span<HistoryBlock> blocks(history.blocks.data() + record.firstBlock, record.blockCount);
    const std::span<HistoryTile> tiles(history.tiles.data() + record.firstTile, record.tileCount);

    // An upload placed but not recorded yet would land after the swap. A tile deleted for being empty comes back clear
    // for its blocks; its clear copy is recorded after the ops of the same submit, so the swap waits for the next one.
    bool ready = true;
    const auto want = [&](TileKey key, bool recreate) {
        const auto found = state.tileIndices.find(key);
        if (found == state.tileIndices.end()) {
            if (recreate) {
                const Tile tile{
                    .canvas = canvas,
                    .mip = 0,
                    .layer = static_cast<Layer>(key >> kTileLayerBitOffset),
                    .gridPos = TileKeyGridPos(key),
                };
                ready = false;
                if (!TileNew(tile)) {
                    MIDORI_LOG_WARN("history: tile {:#018x} not created again", key);
                }
            }
            return;
        }
        TileState& tile = state.tiles[found->second];
        tile.visibleFrame = gAppState.frame;
        ready = ready && HasAnyFlag(tile.flags, TileFlags::Loaded) && !HasAnyFlag(tile.transfer, TileTransfer::Load);
    };
    for (const HistoryBlock& block : blocks) {
        want(block.key, true);
    }
    for (const HistoryTile& tile : tiles) {
        want(tile.key, false);
    }
    if (!ready) {
        if (gAppState.frame - history.pendingFrame >= kHistoryWaitUpdates) {
            MIDORI_LOG_WARN("history: tiles not streamed in after {} updates, {} steps dropped", kHistoryWaitUpdates,
                            history.pending);
            history.pending = 0;
        }
        return;
    }

    // Inserting and deleting move tiles around, so no TileState reference outlives its iteration.
    for (HistoryTile& entry : tiles) {
        const auto found = state.tileIndices.find(entry.key);
        const bool exists = found != state.tileIndices.end();
        if (!exists && entry.cell == kTileAtlasCellInvalid) {
            continue;
        }
        if (!exists) {
            TileState& tile = TileInsert(state, entry.key, std::exchange(entry.cell, kTileAtlasCellInvalid));
            tile.flags |= TileFlags::Loaded | TileFlags::Editable;
            TileAtlasCellOwn(tile.cell, canvas, tile.key);
            // Deleted from the file at the next save no more; the save writes it instead.
            std::erase(state.tileDeletes, entry.key);
            continue;
        }
        if (entry.cell == kTileAtlasCellInvalid) {
            // TileDelete frees only a cell the tile still holds.
            entry.cell = std::exchange(state.tiles[found->second].cell, kTileAtlasCellInvalid);
            TileAtlasCellOwn(entry.cell, kCanvasInvalid, entry.key);
            const Tile tile{
                .canvas = canvas,
                .mip = 0,
                .layer = static_cast<Layer>(entry.key >> kTileLayerBitOffset),
                .gridPos = TileKeyGridPos(entry.key),
            };
            if (!TileDelete(tile)) {
                MIDORI_LOG_WARN("history: tile {:#018x} not deleted", entry.key);
            }
        }
    }
    // A tile's blocks are contiguous.
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const auto found = state.tileIndices.find(blocks[i].key);
        if (found == state.tileIndices.end()) {
            continue;
        }
        TileState& tile = state.tiles[found->second];
        pool.ops.push_back(
            HistoryOp{.slot = blocks[i].slot, .cell = tile.cell, .block = blocks[i].block, .swap = true});
        if (i + 1 < blocks.size() && blocks[i + 1].key == blocks[i].key) {
            continue;
        }
        tile.flags = (tile.flags | TileFlags::Modified) & ~(TileFlags::CachedRam | TileFlags::CachedDisk);
        ++tile.edits;
        std::erase(state.mipBuilds, blocks[i].key);
        TileMipsDelete(canvas, blocks[i].key);
    }
    state.modified = true;
    history.current = undo ? history.current - 1 : history.current + 1;
    history.pending += undo ? 1 : -1;
    history.pendingFrame = gAppState.frame;
}

} // namespace Midori
