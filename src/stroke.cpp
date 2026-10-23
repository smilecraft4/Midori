#include "midori/stroke.hpp"

#include "midori/app.hpp"
#include "midori/history.hpp"
#include "midori/log.hpp"
#include "midori/tile.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

#include <tracy/Tracy.hpp>

namespace Midori {

namespace {

// Keeps a tiny brush from emitting thousands of dabs per canvas unit.
constexpr float kDabSpacingMin = 0.25f;

[[nodiscard]] float DabRadius(const BrushState& brush, float pressure) noexcept {
    return brush.pressureRadius ? brush.radius * pressure : brush.radius;
}

[[nodiscard]] float DabSpacing(const BrushState& brush, float pressure) noexcept {
    return std::max(2.0f * DabRadius(brush, pressure) * brush.spacing, kDabSpacingMin);
}

// How many updates an ended stroke waits for its tiles to stream in before what they missed is dropped.
constexpr std::uint32_t kStrokeWaitUpdates = 300;

[[nodiscard]] std::uint16_t TileTexel(float pos, TileAxis origin) noexcept {
    return static_cast<std::uint16_t>(std::clamp(static_cast<TileAxis>(pos) - origin, 0, kTileSize));
}

// The Morton bits of the 32^2 blocks the texel rect [minX, maxX) x [minY, maxY) overlaps.
[[nodiscard]] std::uint64_t TileBlocksMask(std::uint32_t minX, std::uint32_t minY, std::uint32_t maxX,
                                           std::uint32_t maxY) noexcept {
    std::uint64_t mask = 0;
    if (minX >= maxX || minY >= maxY) {
        return mask;
    }
    for (std::uint32_t y = minY / kTileAtlasBlockSize; y <= (maxY - 1) / kTileAtlasBlockSize; ++y) {
        for (std::uint32_t x = minX / kTileAtlasBlockSize; x <= (maxX - 1) / kTileAtlasBlockSize; ++x) {
            mask |= std::uint64_t{1} << TileBlockIndex(x, y);
        }
    }
    return mask;
}

// Paint from here on goes into the tile: it gets its cell and a backup of what it holds now. A tile the stroke created
// is still clear, so the clear cell serves as its backup.
void StrokeTileAttach(StrokeTile& tile, TileState& tileState, bool created) noexcept {
    ZoneScopedN("StrokeTileAttach");
    tile.cell = tileState.cell;
    if (created) {
        tile.backupCell = kTileAtlasCellClear;
        tile.backedUp = true;
        ++tileState.edits;
        return;
    }
    tile.backupCell = TileAtlasCellTake(TileKeyMip(tile.key), kCanvasInvalid, tile.key);
    if (tile.backupCell == kTileAtlasCellInvalid) {
        MIDORI_LOG_WARN("stroke: no atlas cell to back up tile {:#018x}, it stays unpainted", tile.key);
        return;
    }
    tileState.flags = (tileState.flags | TileFlags::Modified) & ~TileFlags::CachedDisk;
    ++tileState.edits;
}

void DabAdd(StrokeState& stroke, StrokePoint point) noexcept {
    ZoneScopedN("DabAdd");
    if (stroke.dabs.size() >= kStrokeDabMax) {
        return;
    }
    const BrushState& brush = stroke.brush;
    const StrokeDab dab{
        .x = static_cast<float>(point.x - (static_cast<double>(stroke.origin.x) * kTileSize)),
        .y = static_cast<float>(point.y - (static_cast<double>(stroke.origin.y) * kTileSize)),
        .radius = DabRadius(brush, point.pressure),
        .hardness = brush.hardness,
        .flow = brush.flow,
        .opacity = brush.pressureOpacity ? brush.opacity * point.pressure : brush.opacity,
    };
    const auto index = static_cast<std::uint32_t>(stroke.dabs.size());
    stroke.dabs.push_back(dab);

    CanvasState& canvas = gAppState.canvases[stroke.canvas];
    const float reach = dab.radius + 1.0f;
    const auto tileOf = [](float pos) {
        return static_cast<TileAxis>(std::floor(pos / static_cast<float>(kTileSize)));
    };
    for (TileAxis y = tileOf(dab.y - reach); y <= tileOf(dab.y + reach); ++y) {
        for (TileAxis x = tileOf(dab.x - reach); x <= tileOf(dab.x + reach); ++x) {
            const TileGridPos gridPos{.x = stroke.origin.x + x, .y = stroke.origin.y + y};
            const TileKey key = TileKeyMake(stroke.layer, 0, gridPos);
            auto found = canvas.tileIndices.find(key);
            const bool created = found == canvas.tileIndices.end();
            if (created) {
                // Erasing nothing leaves nothing.
                if (stroke.mode == StrokeMode::Erase) {
                    continue;
                }
                const Tile tile{.canvas = stroke.canvas, .mip = 0, .layer = stroke.layer, .gridPos = gridPos};
                if (!TileNew(tile)) {
                    continue;
                }
                found = canvas.tileIndices.find(key);
            }
            auto tile = std::ranges::find(stroke.tiles, key, &StrokeTile::key);
            if (tile == stroke.tiles.end()) {
                // Its level 1-3 tiles are stale from the first dab; they are rebuilt once the stroke is released.
                // Deleting them moves tiles around, so the tile is looked up again.
                TileMipsDelete(stroke.canvas, key);
                found = canvas.tileIndices.find(key);
                stroke.tiles.push_back(StrokeTile{
                    .key = key,
                    .touched = 0,
                    .firstDab = index,
                    .cell = kTileAtlasCellInvalid,
                    .backupCell = kTileAtlasCellInvalid,
                    .dirtyMinX = kTileSize,
                    .dirtyMinY = kTileSize,
                    .dirtyMaxX = 0,
                    .dirtyMaxY = 0,
                    .backedUp = false,
                });
                tile = std::prev(stroke.tiles.end());
                // A tile not streamed in yet waits, see StrokeUpdate: the GPU recomputes every dab since firstDab
                // from its backup, so it joins late without losing any.
                TileState& tileState = canvas.tiles[found->second];
                if (tileState.cell != kTileAtlasCellInvalid) {
                    // A dab can reach past the view; taking the backup cell must not evict the tile itself.
                    tileState.visibleFrame = gAppState.frame;
                    StrokeTileAttach(*tile, tileState, created);
                }
            }
            TileState& tileState = canvas.tiles[found->second];
            tileState.visibleFrame = gAppState.frame;
            if (tile->cell != kTileAtlasCellInvalid && tile->backupCell == kTileAtlasCellInvalid) {
                continue;
            }
            const TileAxis originX = x * kTileSize;
            const TileAxis originY = y * kTileSize;
            const std::uint16_t minX = TileTexel(std::floor(dab.x - reach), originX);
            const std::uint16_t minY = TileTexel(std::floor(dab.y - reach), originY);
            const std::uint16_t maxX = TileTexel(std::ceil(dab.x + reach), originX);
            const std::uint16_t maxY = TileTexel(std::ceil(dab.y + reach), originY);
            tile->dirtyMinX = std::min(tile->dirtyMinX, minX);
            tile->dirtyMinY = std::min(tile->dirtyMinY, minY);
            tile->dirtyMaxX = std::max(tile->dirtyMaxX, maxX);
            tile->dirtyMaxY = std::max(tile->dirtyMaxY, maxY);
            tile->touched |= TileBlocksMask(minX, minY, maxX, maxY);
            if (tile->cell != kTileAtlasCellInvalid) {
                ++tileState.edits;
            }
            canvas.modified = true;
        }
    }
}

} // namespace

void StrokeBegin(const StrokeBeginInfo& beginInfo) noexcept {
    ZoneScopedN("StrokeBegin");
    StrokeState& stroke = gAppState.stroke;
    MIDORI_ASSERT(stroke.canvas == kCanvasInvalid, "stroke began over another");
    stroke.brush = beginInfo.brush;
    stroke.color = beginInfo.color;
    stroke.last = beginInfo.point;
    stroke.toNextDab = DabSpacing(beginInfo.brush, beginInfo.point.pressure);
    stroke.origin = TileGridPos{.x = static_cast<TileAxis>(std::floor(beginInfo.point.x / kTileSize)),
                                .y = static_cast<TileAxis>(std::floor(beginInfo.point.y / kTileSize))};
    stroke.layer = beginInfo.layer;
    stroke.canvas = gAppState.canvasViews[beginInfo.view].canvas;
    stroke.view = beginInfo.view;
    stroke.mode = beginInfo.mode;
    stroke.blendMode = beginInfo.blendMode;
    stroke.ended = false;
    DabAdd(stroke, beginInfo.point);
}

void StrokeTo(StrokePoint point) noexcept {
    ZoneScopedN("StrokeTo");
    StrokeState& stroke = gAppState.stroke;
    MIDORI_ASSERT(stroke.canvas != kCanvasInvalid && !stroke.ended, "no stroke to extend");
    const StrokePoint from = stroke.last;
    const double length = std::hypot(point.x - from.x, point.y - from.y);
    double along = stroke.toNextDab;
    while (along <= length) {
        const double t = along / length;
        const StrokePoint dab{
            .x = std::lerp(from.x, point.x, t),
            .y = std::lerp(from.y, point.y, t),
            .pressure = std::lerp(from.pressure, point.pressure, static_cast<float>(t)),
        };
        DabAdd(stroke, dab);
        along += DabSpacing(stroke.brush, dab.pressure);
    }
    stroke.toNextDab = static_cast<float>(along - length);
    stroke.last = point;
}

void StrokeEnd() noexcept {
    ZoneScopedN("StrokeEnd");
    gAppState.stroke.ended = true;
    gAppState.stroke.endFrame = gAppState.frame;
}

void StrokeUpdate() noexcept {
    ZoneScopedN("StrokeUpdate");
    StrokeState& stroke = gAppState.stroke;
    if (stroke.canvas == kCanvasInvalid) {
        return;
    }
    CanvasState& canvas = gAppState.canvases[stroke.canvas];
    bool waiting = false;
    for (StrokeTile& tile : stroke.tiles) {
        if (tile.cell != kTileAtlasCellInvalid) {
            continue;
        }
        const auto found = canvas.tileIndices.find(tile.key);
        if (found == canvas.tileIndices.end()) {
            continue;
        }
        TileState& tileState = canvas.tiles[found->second];
        // Wanted every update, so streaming loads it.
        tileState.visibleFrame = gAppState.frame;
        if (tileState.cell != kTileAtlasCellInvalid) {
            StrokeTileAttach(tile, tileState, false);
        } else {
            waiting = true;
        }
    }
    if (!stroke.ended || std::ranges::any_of(stroke.tiles, StrokeTilePending)) {
        return;
    }
    if (waiting && gAppState.frame - stroke.endFrame < kStrokeWaitUpdates) {
        return;
    }
    // The push frees slots that ops not recorded yet may name, then takes them again.
    if (!gAppState.historyPool.ops.empty()) {
        return;
    }
    if (waiting) {
        MIDORI_LOG_WARN("stroke: tiles not streamed in after {} updates stay unpainted", kStrokeWaitUpdates);
    }
    HistoryPushStroke(stroke);
    StrokeRelease();
}

void StrokeRelease() noexcept {
    ZoneScopedN("StrokeRelease");
    StrokeState& stroke = gAppState.stroke;
    // The GPU may still read a backup in a frame in flight; its next fill is recorded after that frame.
    for (const StrokeTile& tile : stroke.tiles) {
        if (tile.backupCell != kTileAtlasCellInvalid && tile.backupCell != kTileAtlasCellClear) {
            TileAtlasCellFree(tile.backupCell);
        }
    }
    stroke.dabs.clear();
    stroke.tiles.clear();
    stroke.canvas = kCanvasInvalid;
    stroke.view = kCanvasViewInvalid;
    stroke.ended = false;
}

} // namespace Midori
