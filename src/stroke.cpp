#include "midori/stroke.hpp"

#include "midori/app.hpp"
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

// Paint from here on goes into the tile: it gets its cell and a backup of what it holds now.
void StrokeTileAttach(StrokeTile& tile, TileState& tileState) noexcept {
    tile.cell = tileState.cell;
    tile.backupCell = TileAtlasCellTake(TileKeyMip(tile.key), kCanvasInvalid, tile.key);
    if (tile.backupCell == kTileAtlasCellInvalid) {
        MIDORI_LOG_WARN("stroke: no atlas cell to back up tile {:#018x}, it stays unpainted", tile.key);
        return;
    }
    tileState.flags = (tileState.flags | TileFlags::Modified) & ~TileFlags::CachedDisk;
    ++tileState.edits;
}

void DabAdd(StrokeState& stroke, StrokePoint point) noexcept {
    if (stroke.dabs.size() >= kStrokeDabMax) {
        return;
    }
    const BrushState& brush = stroke.brush;
    const StrokeDab dab{
        .x = point.x,
        .y = point.y,
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
            const TileGridPos gridPos{.x = x, .y = y};
            const TileKey key = TileKeyMake(stroke.layer, 0, gridPos);
            auto found = canvas.tileIndices.find(key);
            if (found == canvas.tileIndices.end()) {
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
                    StrokeTileAttach(*tile, tileState);
                }
            }
            TileState& tileState = canvas.tiles[found->second];
            tileState.visibleFrame = gAppState.frame;
            if (tile->cell != kTileAtlasCellInvalid && tile->backupCell == kTileAtlasCellInvalid) {
                continue;
            }
            const TileAxis originX = x * kTileSize;
            const TileAxis originY = y * kTileSize;
            tile->dirtyMinX = std::min(tile->dirtyMinX, TileTexel(std::floor(dab.x - reach), originX));
            tile->dirtyMinY = std::min(tile->dirtyMinY, TileTexel(std::floor(dab.y - reach), originY));
            tile->dirtyMaxX = std::max(tile->dirtyMaxX, TileTexel(std::ceil(dab.x + reach), originX));
            tile->dirtyMaxY = std::max(tile->dirtyMaxY, TileTexel(std::ceil(dab.y + reach), originY));
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
    const float length = std::hypot(point.x - from.x, point.y - from.y);
    float along = stroke.toNextDab;
    while (along <= length) {
        const float t = along / length;
        const StrokePoint dab{
            .x = std::lerp(from.x, point.x, t),
            .y = std::lerp(from.y, point.y, t),
            .pressure = std::lerp(from.pressure, point.pressure, t),
        };
        DabAdd(stroke, dab);
        along += DabSpacing(stroke.brush, dab.pressure);
    }
    stroke.toNextDab = along - length;
    stroke.last = point;
}

void StrokeEnd() noexcept {
    gAppState.stroke.ended = true;
    gAppState.stroke.endFrame = gAppState.frame;
}

void StrokeUpdate() noexcept {
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
            StrokeTileAttach(tile, tileState);
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
    if (waiting) {
        MIDORI_LOG_WARN("stroke: tiles not streamed in after {} updates stay unpainted", kStrokeWaitUpdates);
    }
    StrokeRelease();
}

void StrokeRelease() noexcept {
    StrokeState& stroke = gAppState.stroke;
    // The GPU may still read a backup in a frame in flight; its next fill is recorded after that frame.
    for (const StrokeTile& tile : stroke.tiles) {
        if (tile.backupCell != kTileAtlasCellInvalid) {
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
