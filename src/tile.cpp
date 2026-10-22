#include "midori/tile.hpp"

#include "midori/app.hpp"
#include "midori/log.hpp"

#include <vector>

#include <tracy/Tracy.hpp>

namespace Midori {

TileAtlasCell TileAtlasCellTake() noexcept {
    std::vector<TileAtlasCell>& freeCells = gAppState.freeAtlasCells;
    if (!freeCells.empty()) {
        const TileAtlasCell cell = freeCells.back();
        freeCells.pop_back();
        return cell;
    }
    TileState* oldest = nullptr;
    for (CanvasState& canvas : gAppState.canvases) {
        if (canvas.id == kCanvasInvalid) {
            continue;
        }
        for (TileState& tile : canvas.tiles) {
            if (HasAnyFlag(tile.flags, TileFlags::Loaded) && tile.visibleFrame != gAppState.frame
                && TilePersisted(tile.flags) && (!oldest || tile.visibleFrame < oldest->visibleFrame)
                && !StrokeHasTile(gAppState.stroke, canvas.id, tile.key)) {
                oldest = &tile;
            }
        }
    }
    if (!oldest) {
        return kTileAtlasCellInvalid;
    }
    // The GPU may still read the cell in a frame in flight; its next fill is recorded after that frame.
    const TileAtlasCell cell = oldest->cell;
    oldest->cell = kTileAtlasCellInvalid;
    oldest->flags &= ~(TileFlags::Loaded | TileFlags::Editable);
    return cell;
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
    const TileAtlasCell cell = TileAtlasCellTake();
    if (cell == kTileAtlasCellInvalid) {
        MIDORI_LOG_WARN("TileNew: all {} atlas cells hold visible or unsaved tiles", kTileAtlasCellMax - 1);
        return std::unexpected(Error::TileAtlasFull);
    }
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
    canvas.creates.push_back(key);
    ++canvas.layers[tile.layer].tileCount;
    canvas.modified = true;
    return {};
}

Result<void> TileDelete(Tile tile) noexcept {
    ZoneScopedN("TileDelete");
    MIDORI_ASSERT(tile.canvas < kCanvasMax && gAppState.canvases[tile.canvas].id == tile.canvas, "invalid canvas {}",
                  tile.canvas);
    CanvasState& canvas = gAppState.canvases[tile.canvas];
    const TileKey key = TileKeyMake(tile.layer, tile.mip, tile.gridPos);
    const auto it = canvas.tileIndices.find(key);
    if (it == canvas.tileIndices.end()) {
        return {};
    }

    const std::uint32_t index = it->second;
    canvas.tileIndices.erase(it);
    // The GPU may still read the cell in a frame in flight; its next fill is recorded after that frame.
    if (canvas.tiles[index].cell != kTileAtlasCellInvalid) {
        gAppState.freeAtlasCells.push_back(canvas.tiles[index].cell);
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
