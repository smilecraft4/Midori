#include "midori/stroke.hpp"

#include "midori/app.hpp"
#include "midori/history.hpp"
#include "midori/layer.hpp"
#include "midori/log.hpp"
#include "midori/tile.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <span>
#include <vector>

#include <tracy/Tracy.hpp>

namespace Midori {

namespace {

float BrushInputValue(BrushInput input, const MdslDab& inputs) noexcept {
    switch (input) {
        case BrushInput::Pressure:
            return inputs.pressure;
        case BrushInput::TiltX:
            return (inputs.tiltX + 1.0f) * 0.5f;
        case BrushInput::TiltY:
            return (inputs.tiltY + 1.0f) * 0.5f;
        case BrushInput::Speed:
            return std::hypot(inputs.velocityX, inputs.velocityY) / kStrokeSpeedFull;
        case BrushInput::Random:
            return inputs.random;
        case BrushInput::Count_:
            break;
    }
    return 1.0f;
}

// How many updates an ended stroke waits for its tiles to stream in before what they missed is dropped.
constexpr std::uint32_t kStrokeWaitUpdates = 300;
// DIPs of low-pass filter distance per unit of smoothing above 1, and DIPs between Catmull-Rom knots per unit.
constexpr double kStrokeSmoothingFilterDips = 3.0;
constexpr double kStrokeSmoothingKnotDips = 1.5;
constexpr double kStrokeSplineStepDips = 1.5;
constexpr int kStrokeSplineStepsMax = 64;

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

// 0..1, the same for the same seed and stamp, so a stroke's stamps keep theirs. PCG's output hash.
float StampRandom(std::uint32_t seed, std::uint32_t stamp) noexcept {
    std::uint32_t h = ((seed + stamp) * 747796405U) + 2891336453U;
    h = ((h >> ((h >> 28U) + 4U)) ^ h) * 277803737U;
    h = (h >> 22U) ^ h;
    return static_cast<float>(h >> 8U) * 0x1p-24f;
}

// The inputs at `point`, the stroke's at its last point before: the velocity follows the pen over
// kStrokeVelocitySmoothing, and the direction keeps the last one while the pen does not move.
MdslDab InputsAt(const StrokeState& stroke, const StrokePoint& point) noexcept {
    const MdslDab& before = stroke.inputs;
    MdslDab inputs = before;
    inputs.pressure = point.pressure;
    inputs.tiltX = point.tiltX;
    inputs.tiltY = point.tiltY;
    inputs.time = static_cast<float>(std::max(point.time - stroke.first.time, 0.0));
    const double dx = point.x - stroke.last.x;
    const double dy = point.y - stroke.last.y;
    const double length = std::hypot(dx, dy);
    inputs.along = before.along + static_cast<float>(length);
    if (length > 0.0) {
        inputs.directionX = static_cast<float>(dx / length);
        inputs.directionY = static_cast<float>(dy / length);
    }
    // Clocks count in milliseconds, so points can share a time: they keep the velocity until one does not.
    if (const double dt = point.time - stroke.last.time; dt > 0.0) {
        const auto follow = static_cast<float>(1.0 - std::exp(-dt / kStrokeVelocitySmoothing));
        inputs.velocityX = std::lerp(before.velocityX, static_cast<float>(dx / dt), follow);
        inputs.velocityY = std::lerp(before.velocityY, static_cast<float>(dy / dt), follow);
    }
    return inputs;
}

// Its distance to the next stamp, which takes `inputs` and the stroke's next random value. The stamp is dropped once
// the stroke holds kStrokeDabMax.
float DabAdd(StrokeState& stroke, double x, double y, MdslDab inputs) noexcept {
    ZoneScopedN("DabAdd");
    inputs.random = StampRandom(stroke.seed, stroke.stamps++);
    const StrokeStamp stamp = StrokeStampAt(gAppState.tools.shaders[stroke.shader].brush, stroke.brush, stroke.curves,
                                            inputs);
    if (stroke.dabs.size() >= kStrokeDabMax) {
        return stamp.spacing;
    }
    const StrokeDab dab{
        .x = static_cast<float>(x + stamp.offsetX - (static_cast<double>(stroke.origin.x) * kTileSize)),
        .y = static_cast<float>(y + stamp.offsetY - (static_cast<double>(stroke.origin.y) * kTileSize)),
        .radius = stamp.radius,
        .width = 0.0f,
        .height = 0.0f,
        .inputs = inputs,
        .params = stamp.params,
    };
    const auto index = static_cast<std::uint32_t>(stroke.dabs.size());
    stroke.dabs.push_back(dab);
    const float reach = dab.radius + 1.0f;
    BoxTouch(stroke, stroke.layer, dab.x - reach, dab.y - reach, dab.x + reach, dab.y + reach, index,
             StrokeCreates(stroke.mode));
    return stamp.spacing;
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
        .width = static_cast<float>(std::abs(point.x - stroke.first.x)),
        .height = static_cast<float>(std::abs(point.y - stroke.first.y)),
        .inputs = {},
        .params = {},
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

// One stamp for the whole fill, at the stroke's inputs now: its radius is how far in from the edge the brush reaches.
StrokeDab FillStamp(StrokeState& stroke) noexcept {
    MdslDab inputs = stroke.inputs;
    inputs.random = StampRandom(stroke.seed, 0);
    const StrokeStamp stamp =
        StrokeStampAt(gAppState.tools.shaders[stroke.shader].brush, stroke.brush, stroke.curves, inputs);
    return StrokeDab{
        .x = 0.0f,
        .y = 0.0f,
        .radius = stamp.radius,
        .width = 0.0f,
        .height = 0.0f,
        .inputs = inputs,
        .params = stamp.params,
    };
}

// The fill's corners' box and a texel around: the brush stops at the shape's edge. Fewer than 3 corners cover nothing.
void FillTouch(StrokeState& stroke) noexcept {
    if (stroke.dabs.size() < 4) {
        return;
    }
    const auto corners = std::span(stroke.dabs).subspan(1);
    BoxTouch(stroke, stroke.layer, std::ranges::min(corners, {}, &StrokeDab::x).x - 1.0f,
             std::ranges::min(corners, {}, &StrokeDab::y).y - 1.0f,
             std::ranges::max(corners, {}, &StrokeDab::x).x + 1.0f,
             std::ranges::max(corners, {}, &StrokeDab::y).y + 1.0f, 0, StrokeCreates(stroke.mode));
}

// A corner in canvas units from the stroke's origin tile.
StrokeDab FillCorner(const StrokeState& stroke, double x, double y) noexcept {
    return StrokeDab{
        .x = static_cast<float>(x - (static_cast<double>(stroke.origin.x) * kTileSize)),
        .y = static_cast<float>(y - (static_cast<double>(stroke.origin.y) * kTileSize)),
        .radius = 0.0f,
        .width = 0.0f,
        .height = 0.0f,
        .inputs = {},
        .params = {},
    };
}

// The stamp follows the inputs, so every move recomputes the whole shape, and where it was so shrinking puts back what
// it had covered. A lasso takes the point as a corner kStrokeLassoSpacing screen pixels past the last one.
void FillTo(StrokeState& stroke, StrokePoint point) noexcept {
    ZoneScopedN("FillTo");
    FillTouch(stroke);
    stroke.inputs = InputsAt(stroke, point);
    stroke.last = point;
    if (stroke.shape == StrokeShape::FillRect) {
        const StrokePoint& first = stroke.first;
        stroke.dabs.resize(1);
        stroke.dabs.push_back(FillCorner(stroke, first.x, first.y));
        stroke.dabs.push_back(FillCorner(stroke, point.x, first.y));
        stroke.dabs.push_back(FillCorner(stroke, point.x, point.y));
        stroke.dabs.push_back(FillCorner(stroke, first.x, point.y));
    } else {
        const StrokeDab corner = FillCorner(stroke, point.x, point.y);
        const StrokeDab& last = stroke.dabs.back();
        const double zoom = std::abs(gAppState.canvasViews[stroke.view].viewport.zoomX);
        if (stroke.dabs.size() <= kStrokeLassoPointsMax
            && std::hypot(corner.x - last.x, corner.y - last.y) * zoom >= kStrokeLassoSpacing) {
            stroke.dabs.push_back(corner);
        }
    }
    stroke.dabs.front() = FillStamp(stroke);
    FillTouch(stroke);
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
        .width = 0.0f,
        .height = 0.0f,
        .inputs = {},
        .params = {},
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

// Steps dabs along the straight segment from `stroke.last` to `point`.
void StrokeSegment(StrokeState& stroke, const StrokePoint& point) noexcept {
    const StrokePoint from = stroke.last;
    const MdslDab before = stroke.inputs;
    const MdslDab after = InputsAt(stroke, point);
    const double length = std::hypot(point.x - from.x, point.y - from.y);
    double along = stroke.toNextDab;
    while (along <= length) {
        const double t = length > 0.0 ? along / length : 0.0;
        const auto at = static_cast<float>(t);
        const MdslDab inputs{
            .pressure = std::lerp(before.pressure, after.pressure, at),
            .tiltX = std::lerp(before.tiltX, after.tiltX, at),
            .tiltY = std::lerp(before.tiltY, after.tiltY, at),
            .velocityX = std::lerp(before.velocityX, after.velocityX, at),
            .velocityY = std::lerp(before.velocityY, after.velocityY, at),
            .directionX = after.directionX,
            .directionY = after.directionY,
            .random = 0.0f,
            .along = before.along + static_cast<float>(along),
            .time = std::lerp(before.time, after.time, at),
        };
        along += DabAdd(stroke, std::lerp(from.x, point.x, t), std::lerp(from.y, point.y, t), inputs);
    }
    stroke.toNextDab = static_cast<float>(along - length);
    stroke.last = point;
    stroke.inputs = after;
}

[[nodiscard]] double StrokeViewZoom(const StrokeState& stroke) noexcept {
    if (stroke.view == kCanvasViewInvalid) {
        return 1.0;
    }
    const double zoom = std::abs(gAppState.canvasViews[stroke.view].viewport.zoomX);
    return zoom > 0.0 ? zoom : 1.0;
}

// Mirrors `neighbor` across `endpoint`, so an end segment starts or finishes along its chord.
[[nodiscard]] StrokePoint KnotReflect(const StrokePoint& endpoint, const StrokePoint& neighbor) noexcept {
    StrokePoint reflected = endpoint;
    reflected.x = (2.0 * endpoint.x) - neighbor.x;
    reflected.y = (2.0 * endpoint.y) - neighbor.y;
    return reflected;
}

void StrokeSplineSegment(StrokeState& stroke, const StrokePoint& p0, const StrokePoint& p1, const StrokePoint& p2,
                         const StrokePoint& p3, double zoom) noexcept {
    const double chordDips = std::hypot(p2.x - p1.x, p2.y - p1.y) * zoom;
    const int steps =
        std::clamp(static_cast<int>(std::ceil(chordDips / kStrokeSplineStepDips)), 1, kStrokeSplineStepsMax);
    const float tension = std::min(stroke.smoothing, 1.0f);
    for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(steps);
        StrokeSegment(stroke, StrokeCatmullRomAt(p0, p1, p2, p3, t, tension));
    }
}

// Advances the 3-knot window by `knot`, drawing the segment before the lookahead knot once 3 knots are known.
void StrokeKnotPush(StrokeState& stroke, const StrokePoint& knot, double zoom) noexcept {
    if (stroke.knotCount == 1) {
        stroke.knots[1] = knot;
        stroke.knotCount = 2;
        return;
    }
    if (stroke.knotCount == 2) {
        stroke.knots[2] = knot;
        stroke.knotCount = 3;
        StrokeSplineSegment(stroke, KnotReflect(stroke.knots[0], stroke.knots[1]), stroke.knots[0], stroke.knots[1],
                            stroke.knots[2], zoom);
        return;
    }
    StrokeSplineSegment(stroke, stroke.knots[0], stroke.knots[1], stroke.knots[2], knot, zoom);
    stroke.knots[0] = stroke.knots[1];
    stroke.knots[1] = stroke.knots[2];
    stroke.knots[2] = knot;
}

} // namespace

void StrokeBegin(const StrokeBeginInfo& beginInfo) noexcept {
    ZoneScopedN("StrokeBegin");
    StrokeState& stroke = gAppState.stroke;
    MIDORI_ASSERT(stroke.canvas == kCanvasInvalid, "stroke began over another");
    stroke.brush = beginInfo.brush;
    stroke.shader = beginInfo.shader;
    BrushCurvesBake(stroke.brush, stroke.curves);
    stroke.color = beginInfo.color;
    stroke.first = beginInfo.point;
    stroke.last = beginInfo.point;
    stroke.filtered = beginInfo.point;
    stroke.tail = beginInfo.point;
    stroke.knots = {beginInfo.point, {}, {}};
    stroke.knotCount = 1;
    stroke.smoothing = std::clamp(beginInfo.smoothing, 0.0f, kToolSmoothingMax);
    stroke.smoothingTail = std::clamp(beginInfo.smoothingTail, 0.0f, 1.0f);
    stroke.inputs = MdslDab{
        .pressure = beginInfo.point.pressure,
        .tiltX = beginInfo.point.tiltX,
        .tiltY = beginInfo.point.tiltY,
        .velocityX = 0.0f,
        .velocityY = 0.0f,
        .directionX = 1.0f,
        .directionY = 0.0f,
        .random = 0.0f,
        .along = 0.0f,
        .time = 0.0f,
    };
    stroke.toNextDab = 0.0f;
    stroke.seed = static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    stroke.stamps = 0;
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
        stroke.toNextDab = DabAdd(stroke, beginInfo.point.x, beginInfo.point.y, stroke.inputs);
    } else if (stroke.shape == StrokeShape::Lasso) {
        LassoAdd(stroke, beginInfo.point);
    } else if (stroke.shape == StrokeShape::FillRect || stroke.shape == StrokeShape::FillLasso) {
        stroke.dabs.push_back(FillStamp(stroke));
        if (stroke.shape == StrokeShape::FillLasso) {
            stroke.dabs.push_back(FillCorner(stroke, beginInfo.point.x, beginInfo.point.y));
        }
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
    if (stroke.shape == StrokeShape::FillRect || stroke.shape == StrokeShape::FillLasso) {
        FillTo(stroke, point);
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
    if (stroke.smoothing <= 0.0f) {
        StrokeSegment(stroke, point);
        return;
    }
    stroke.tail = point;
    const double zoom = StrokeViewZoom(stroke);
    const double filterLength =
        std::max(static_cast<double>(stroke.smoothing) - 1.0, 0.0) * kStrokeSmoothingFilterDips / zoom;
    const double distance = std::hypot(point.x - stroke.filtered.x, point.y - stroke.filtered.y);
    const double follow = filterLength > 0.0 ? (1.0 - std::exp(-distance / filterLength)) : 1.0;
    const auto followF = static_cast<float>(follow);
    stroke.filtered = StrokePoint{
        .x = std::lerp(stroke.filtered.x, point.x, follow),
        .y = std::lerp(stroke.filtered.y, point.y, follow),
        .time = point.time,
        .pressure = std::lerp(stroke.filtered.pressure, point.pressure, followF),
        .tiltX = std::lerp(stroke.filtered.tiltX, point.tiltX, followF),
        .tiltY = std::lerp(stroke.filtered.tiltY, point.tiltY, followF),
    };
    const StrokePoint& lastKnot = stroke.knots[stroke.knotCount - 1];
    const double knotSpacing = std::max(1.0, static_cast<double>(stroke.smoothing) * kStrokeSmoothingKnotDips) / zoom;
    if (std::hypot(stroke.filtered.x - lastKnot.x, stroke.filtered.y - lastKnot.y) >= knotSpacing) {
        StrokeKnotPush(stroke, stroke.filtered, zoom);
    }
}

StrokeStamp StrokeStampAt(const MdslBrush& source, const BrushState& brush,
                          std::span<const float, kMdslCurveFloats> curves, const MdslDab& inputs) noexcept {
    StrokeStamp stamp{};
    const std::vector<MdslParam>& params = source.params;
    for (std::size_t i = 0; i < params.size(); ++i) {
        const BrushParam& param = brush.params[i];
        float scale = 1.0f;
        for (std::size_t input = 0; input < kBrushInputCount; ++input) {
            if (BrushParamUses(param, static_cast<BrushInput>(input))) {
                scale *= CurveEval(param.curves[input], BrushInputValue(static_cast<BrushInput>(input), inputs));
            }
        }
        for (std::uint32_t c = 0; c < MdslComponents(params[i].type); ++c) {
            stamp.params[params[i].offset + c] = param.value[c] * scale;
        }
    }
    const MdslStamp result = MdslStampRun(source.stamp, MdslStampInput{
                                                                     .params = stamp.params,
                                                                     .curves = curves,
                                                                     .dab = inputs,
                                                                 });
    // Written so a NaN fails each test and takes the bound.
    stamp.radius = result.radius > 0.0f ? std::min(result.radius, kStrokeStampRadiusMax) : 0.0f;
    stamp.spacing = result.spacing >= kStrokeStampSpacingMin ? result.spacing : kStrokeStampSpacingMin;
    const auto offset = [](float value) {
        return std::isnan(value) ? 0.0f : std::clamp(value, -kStrokeStampOffsetMax, kStrokeStampOffsetMax);
    };
    stamp.offsetX = offset(result.offsetX);
    stamp.offsetY = offset(result.offsetY);
    return stamp;
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
    StrokeState& stroke = gAppState.stroke;
    if (!stroke.ended && stroke.shape == StrokeShape::Brush && stroke.smoothing > 0.0f && stroke.smoothingTail > 0.0f
        && stroke.knotCount > 0) {
        const StrokePoint p1 = stroke.last;
        const StrokePoint p2{
            .x = stroke.filtered.x,
            .y = stroke.filtered.y,
            .time = stroke.tail.time,
            .pressure = stroke.tail.pressure,
            .tiltX = stroke.tail.tiltX,
            .tiltY = stroke.tail.tiltY,
        };
        const double chord = std::hypot(p2.x - p1.x, p2.y - p1.y);
        if (chord > 1e-4) {
            const StrokePoint p0 = stroke.knotCount == 3 ? stroke.knots[0] : KnotReflect(p1, p2);
            const double chordDips = chord * static_cast<double>(stroke.smoothingTail) * StrokeViewZoom(stroke);
            const int steps =
                std::clamp(static_cast<int>(std::ceil(chordDips / kStrokeSplineStepDips)), 1, kStrokeSplineStepsMax);
            const float tension = std::min(stroke.smoothing, 1.0f);
            for (int i = 1; i <= steps; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(steps);
                StrokeSegment(stroke, StrokeCatmullRomTailAt(p0, p1, p2, t, stroke.smoothingTail, tension));
            }
        }
    }
    stroke.ended = true;
    stroke.endFrame = gAppState.frame;
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
