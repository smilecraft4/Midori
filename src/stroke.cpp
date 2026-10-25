#include "midori/stroke.hpp"

#include "midori/app.hpp"
#include "midori/history.hpp"
#include "midori/layer.hpp"
#include "midori/log.hpp"
#include "midori/tile.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

#include <tracy/Tracy.hpp>

namespace Midori {

namespace {

// Keeps a tiny brush from emitting thousands of dabs per canvas unit.
constexpr float kDabSpacingMin = 0.25f;
// One texel at level 0, the narrowest a dab's mask gets.
constexpr float kDabRadiusSolid = 1.0f;

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

// The level 1-3 tiles of a tile the stroke reaches for the first time. The loaded ones follow its paint, see
// RecordStroke: edited from now on, so a download requested before does not count. The others would come back stale
// and are deleted, built again once the stroke is released. Deleting moves tiles around.
void StrokeMipsPrepare(Canvas canvasId, TileKey key) noexcept {
    ZoneScopedN("StrokeMipsPrepare");
    CanvasState& canvas = gAppState.canvases[canvasId];
    for (TileMip mip = 1; mip < kTileMipMax; ++mip) {
        const auto found = canvas.tileIndices.find(TileKeyWithMip(key, mip));
        if (found == canvas.tileIndices.end()) {
            continue;
        }
        TileState& tile = canvas.tiles[found->second];
        if (tile.cell != kTileAtlasCellInvalid) {
            tile.flags = (tile.flags | TileFlags::Modified) & ~TileFlags::CachedDisk;
            ++tile.edits;
            continue;
        }
        const Tile stale{
            .canvas = canvasId,
            .mip = mip,
            .layer = static_cast<Layer>(key >> kTileLayerBitOffset),
            .gridPos = TileKeyGridPos(key),
        };
        if (!TileDelete(stale)) {
            MIDORI_LOG_WARN("stroke: stale tile {:#018x} level {} not deleted", key, mip);
        }
    }
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

// The tile of `layer` at `gridPos` takes in the texels [minX, maxX) x [minY, maxY): they are recomputed from the dabs
// since `firstDab` from its backup. A missing tile is created when `create`.
void TileTouch(StrokeState& stroke, Layer layer, TileGridPos gridPos, std::uint16_t minX, std::uint16_t minY,
               std::uint16_t maxX, std::uint16_t maxY, std::uint32_t firstDab, bool create) noexcept {
    CanvasState& canvas = gAppState.canvases[stroke.canvas];
    const TileKey key = TileKeyMake(layer, 0, gridPos);
    auto found = canvas.tileIndices.find(key);
    const bool created = found == canvas.tileIndices.end();
    if (created) {
        const Tile tile{.canvas = stroke.canvas, .mip = 0, .layer = layer, .gridPos = gridPos};
        if (!create || !TileNew(tile)) {
            return;
        }
        found = canvas.tileIndices.find(key);
    }
    auto tile = std::ranges::find(stroke.tiles, key, &StrokeTile::key);
    if (tile == stroke.tiles.end()) {
        StrokeMipsPrepare(stroke.canvas, key);
        found = canvas.tileIndices.find(key);
        stroke.tiles.push_back(StrokeTile{
            .key = key,
            .touched = 0,
            .firstDab = firstDab,
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
        return;
    }
    tile->dirtyMinX = std::min(tile->dirtyMinX, minX);
    tile->dirtyMinY = std::min(tile->dirtyMinY, minY);
    tile->dirtyMaxX = std::max(tile->dirtyMaxX, maxX);
    tile->dirtyMaxY = std::max(tile->dirtyMaxY, maxY);
    tile->touched |= TileBlocksMask(minX, minY, maxX, maxY);
    if (tile->cell != kTileAtlasCellInvalid) {
        ++tileState.edits;
        LayerCompositesTouch(canvas, key);
    }
    canvas.modified = true;
}

// Erasing or intersecting nothing leaves nothing.
[[nodiscard]] bool StrokeCreates(StrokeMode mode) noexcept {
    return mode != StrokeMode::Erase && mode != StrokeMode::Intersect;
}

// Every tile of `layer` the box [minX, maxX] x [minY, maxY] overlaps, in canvas units from the stroke's origin tile,
// takes in its texels there.
void BoxTouch(StrokeState& stroke, Layer layer, float minX, float minY, float maxX, float maxY, std::uint32_t firstDab,
              bool create) noexcept {
    const auto tileOf = [](float pos) {
        return static_cast<TileAxis>(std::floor(pos / static_cast<float>(kTileSize)));
    };
    for (TileAxis y = tileOf(minY); y <= tileOf(maxY); ++y) {
        for (TileAxis x = tileOf(minX); x <= tileOf(maxX); ++x) {
            const TileAxis originX = x * kTileSize;
            const TileAxis originY = y * kTileSize;
            TileTouch(stroke, layer, TileGridPos{.x = stroke.origin.x + x, .y = stroke.origin.y + y},
                      TileTexel(std::floor(minX), originX), TileTexel(std::floor(minY), originY),
                      TileTexel(std::ceil(maxX), originX), TileTexel(std::ceil(maxY), originY), firstDab, create);
        }
    }
}

void DabAdd(StrokeState& stroke, StrokePoint point) noexcept {
    ZoneScopedN("DabAdd");
    if (stroke.dabs.size() >= kStrokeDabMax) {
        return;
    }
    const BrushState& brush = stroke.brush;
    const float radius = DabRadius(brush, point.pressure);
    const float opacity =
        brush.pressureOpacity
            ? brush.opacity * std::lerp(brush.pressureOpacityMin, brush.pressureOpacityMax, point.pressure)
            : brush.opacity;
    // A dab's mask is at least a texel wide, so dabs pressure shrinks below one would each still pull the coverage up
    // to the full opacity, packed at the minimum spacing: a hard hairline. Below a texel their cap shrinks with them
    // instead. A brush set that small keeps its own opacity.
    const float fade = std::min(radius / std::min(brush.radius, kDabRadiusSolid), 1.0f);
    const StrokeDab dab{
        .x = static_cast<float>(point.x - (static_cast<double>(stroke.origin.x) * kTileSize)),
        .y = static_cast<float>(point.y - (static_cast<double>(stroke.origin.y) * kTileSize)),
        .radius = radius,
        .hardness = brush.pressureHardness
                      ? brush.hardness * std::lerp(brush.pressureHardnessMin, brush.pressureHardnessMax, point.pressure)
                      : brush.hardness,
        .flow = brush.pressureFlow
                  ? brush.flow * std::lerp(brush.pressureFlowMin, brush.pressureFlowMax, point.pressure)
                  : brush.flow,
        .opacity = opacity * fade,
        .width = 0.0f,
        .height = 0.0f,
    };
    const auto index = static_cast<std::uint32_t>(stroke.dabs.size());
    stroke.dabs.push_back(dab);
    const float reach = dab.radius + 1.0f;
    BoxTouch(stroke, stroke.layer, dab.x - reach, dab.y - reach, dab.x + reach, dab.y + reach, index,
             StrokeCreates(stroke.mode));
}

// The rectangle from the first point to `point` replaces the stroke's one dab. Where the last one was is recomputed
// too, so shrinking it puts back what it had covered.
void RectSet(StrokeState& stroke, StrokePoint point) noexcept {
    ZoneScopedN("RectSet");
    const auto local = [](double pos, TileAxis origin) {
        return static_cast<float>(pos - (static_cast<double>(origin) * kTileSize));
    };
    const StrokeDab dab{
        .x = local(std::min(stroke.first.x, point.x), stroke.origin.x),
        .y = local(std::min(stroke.first.y, point.y), stroke.origin.y),
        .radius = 0.0f,
        .hardness = 1.0f,
        .flow = 1.0f,
        .opacity = 1.0f,
        .width = static_cast<float>(std::abs(point.x - stroke.first.x)),
        .height = static_cast<float>(std::abs(point.y - stroke.first.y)),
    };
    // The edge is antialiased over a texel each side.
    if (!stroke.dabs.empty()) {
        const StrokeDab last = stroke.dabs.front();
        stroke.dabs.front() = dab;
        BoxTouch(stroke, stroke.layer, last.x - 1.0f, last.y - 1.0f, last.x + last.width + 1.0f,
                 last.y + last.height + 1.0f, 0, StrokeCreates(stroke.mode));
    } else {
        stroke.dabs.push_back(dab);
    }
    BoxTouch(stroke, stroke.layer, dab.x - 1.0f, dab.y - 1.0f, dab.x + dab.width + 1.0f, dab.y + dab.height + 1.0f, 0,
             StrokeCreates(stroke.mode));
}

// Where the pixels lifted from under the selection's bounds land through `transform`, and a texel around for the
// filtering, in canvas units from the stroke's origin tile: recomputed in the layer and in the selection.
void TransformTouch(StrokeState& stroke, const StrokeTransform& transform) noexcept {
    const SelectionBounds& source = stroke.selectionBefore;
    const SelectionBounds box = StrokeTransformBounds(transform, SelectionBounds{
                                                                     .minX = source.minX - 1.0,
                                                                     .minY = source.minY - 1.0,
                                                                     .maxX = source.maxX + 1.0,
                                                                     .maxY = source.maxY + 1.0,
                                                                 });
    const double originX = static_cast<double>(stroke.origin.x) * kTileSize;
    const double originY = static_cast<double>(stroke.origin.y) * kTileSize;
    const Layer selection = gAppState.canvases[stroke.canvas].selection;
    for (const Layer layer : {stroke.layer, selection}) {
        BoxTouch(stroke, layer, static_cast<float>(box.minX - originX), static_cast<float>(box.minY - originY),
                 static_cast<float>(box.maxX - originX), static_cast<float>(box.maxY - originY), 0, true);
    }
}

// A lasso point is a dab, the polygon's next corner. Only the triangle between the last point, it and the first, where
// the polygon closed before, changes: that box a texel wider is recomputed.
void LassoAdd(StrokeState& stroke, StrokePoint point) noexcept {
    stroke.dabs.push_back(StrokeDab{
        .x = static_cast<float>(point.x - (static_cast<double>(stroke.origin.x) * kTileSize)),
        .y = static_cast<float>(point.y - (static_cast<double>(stroke.origin.y) * kTileSize)),
        .radius = 0.0f,
        .hardness = 1.0f,
        .flow = 1.0f,
        .opacity = 1.0f,
        .width = 0.0f,
        .height = 0.0f,
    });
    stroke.last = point;
    if (stroke.dabs.size() < 3) {
        return;
    }
    const StrokeDab& first = stroke.dabs.front();
    const StrokeDab& last = stroke.dabs[stroke.dabs.size() - 2];
    const StrokeDab& added = stroke.dabs.back();
    BoxTouch(stroke, stroke.layer, std::min({first.x, last.x, added.x}) - 1.0f,
             std::min({first.y, last.y, added.y}) - 1.0f, std::max({first.x, last.x, added.x}) + 1.0f,
             std::max({first.y, last.y, added.y}) + 1.0f, 0, StrokeCreates(stroke.mode));
}

} // namespace

void StrokeBegin(const StrokeBeginInfo& beginInfo) noexcept {
    ZoneScopedN("StrokeBegin");
    StrokeState& stroke = gAppState.stroke;
    MIDORI_ASSERT(stroke.canvas == kCanvasInvalid, "stroke began over another");
    stroke.brush = beginInfo.brush;
    stroke.color = beginInfo.color;
    stroke.first = beginInfo.point;
    stroke.last = beginInfo.point;
    stroke.toNextDab = DabSpacing(beginInfo.brush, beginInfo.point.pressure);
    stroke.origin = TileGridPos{.x = static_cast<TileAxis>(std::floor(beginInfo.point.x / kTileSize)),
                                .y = static_cast<TileAxis>(std::floor(beginInfo.point.y / kTileSize))};
    stroke.layer = beginInfo.layer;
    stroke.canvas = gAppState.canvasViews[beginInfo.view].canvas;
    stroke.view = beginInfo.view;
    stroke.mode = beginInfo.mode;
    stroke.blendMode = beginInfo.blendMode;
    stroke.shape = beginInfo.shape;
    stroke.transform = {};
    stroke.ended = false;
    stroke.discard = false;
    const CanvasState& canvas = gAppState.canvases[stroke.canvas];
    stroke.selectionBefore = canvas.selectionBounds;
    if (stroke.shape == StrokeShape::Transform) {
        const SelectionBounds& source = stroke.selectionBefore;
        const auto tileOf = [](double pos) {
            return static_cast<TileAxis>(std::floor(pos / kTileSize));
        };
        std::vector<TileKey> lifted;
        for (const TileState& tile : canvas.tiles) {
            const Layer layer = TileKeyLayer(tile.key);
            const TileGridPos gridPos = TileKeyGridPos(tile.key);
            if ((layer == stroke.layer || layer == canvas.selection) && TileKeyMip(tile.key) == 0
                && gridPos.x >= tileOf(source.minX) && gridPos.x <= tileOf(source.maxX)
                && gridPos.y >= tileOf(source.minY) && gridPos.y <= tileOf(source.maxY)) {
                lifted.push_back(tile.key);
            }
        }
        // Touching may create tiles, which moves `canvas.tiles`.
        for (const TileKey key : lifted) {
            TileTouch(stroke, TileKeyLayer(key), TileKeyGridPos(key), 0, 0, kTileSize, kTileSize, 0, false);
        }
        return;
    }
    if (stroke.mode == StrokeMode::Replace || stroke.mode == StrokeMode::Intersect) {
        std::vector<TileGridPos> existing;
        for (const TileState& tile : canvas.tiles) {
            if (TileKeyLayer(tile.key) == stroke.layer && TileKeyMip(tile.key) == 0) {
                existing.push_back(TileKeyGridPos(tile.key));
            }
        }
        // Touching may create tiles, which moves `canvas.tiles`.
        for (const TileGridPos gridPos : existing) {
            TileTouch(stroke, stroke.layer, gridPos, 0, 0, kTileSize, kTileSize, 0, false);
        }
    }
    if (stroke.shape == StrokeShape::Brush) {
        DabAdd(stroke, beginInfo.point);
    } else if (stroke.shape == StrokeShape::Lasso) {
        LassoAdd(stroke, beginInfo.point);
    }
}

void StrokeTo(StrokePoint point) noexcept {
    ZoneScopedN("StrokeTo");
    StrokeState& stroke = gAppState.stroke;
    MIDORI_ASSERT(stroke.canvas != kCanvasInvalid && !stroke.ended, "no stroke to extend");
    if (stroke.shape == StrokeShape::Rect) {
        RectSet(stroke, point);
        stroke.last = point;
        return;
    }
    if (stroke.shape == StrokeShape::Lasso) {
        const double zoom = std::abs(gAppState.canvasViews[stroke.view].viewport.zoomX);
        if (stroke.dabs.size() < kStrokeLassoPointsMax
            && std::hypot(point.x - stroke.last.x, point.y - stroke.last.y) * zoom >= kStrokeLassoSpacing) {
            LassoAdd(stroke, point);
        }
        return;
    }
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

void StrokeTransformTo(const StrokeTransform& transform) noexcept {
    ZoneScopedN("StrokeTransformTo");
    StrokeState& stroke = gAppState.stroke;
    MIDORI_ASSERT(stroke.canvas != kCanvasInvalid && !stroke.ended && stroke.shape == StrokeShape::Transform,
                  "no transform to move");
    TransformTouch(stroke, stroke.transform);
    stroke.transform = transform;
    TransformTouch(stroke, stroke.transform);
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
    // A stroke the selection clips needs its mask tiles too, see RecordStroke.
    if (canvas.selection != kLayerInvalid && stroke.layer != canvas.selection) {
        for (const StrokeTile& tile : stroke.tiles) {
            if (const auto mask = canvas.tileIndices.find(TileKeyMake(canvas.selection, 0, TileKeyGridPos(tile.key)));
                mask != canvas.tileIndices.end()) {
                canvas.tiles[mask->second].visibleFrame = gAppState.frame;
            }
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
    if (!stroke.discard) {
        HistoryPushStroke(stroke);
    }
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
    stroke.discard = false;
}

} // namespace Midori
