#pragma once

#include "midori/canvas.hpp"
#include "midori/color.hpp"
#include "midori/mdsl.hpp"
#include "midori/tile.hpp"
#include "midori/tool.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace Midori {

constexpr std::uint32_t kStrokeDabMax = 1 << 16;

// Mirrors kStroke* in stroke.cs.hlsl.
enum struct StrokeMode : std::uint8_t {
    Paint,
    Erase,
    Replace,   // the color times the coverage, whatever was below: a selection that replaces the last one
    Intersect, // what was below times the coverage
    Exclude,   // what was below where nothing covers, its opposite where the color does
};

enum struct StrokeShape : std::uint8_t {
    Brush,     // dabs along the path
    Rect,      // one dab, the rectangle from the first point to the last
    Lasso,     // the polygon through the dabs, one per point, closed back to the first
    Transform, // no dab: the selected pixels of the layer and the selection with them, placed by StrokeState::transform
    FillRect,  // the brush over a polygon: dabs[0] is its stamp, the rectangle from the first point to the last its
               // four corners after it
    FillLasso, // the same over the polygon through the points, closed back to the first
};

// Canvas units to canvas units, projectively: (xx x + xy y + x, yx x + yy y + y) / (wx x + wy y + w). The divisor
// stays above 0 over the box a transform lifts, a texel around included.
struct StrokeTransform {
    double xx{1.0};
    double xy{};
    double yx{};
    double yy{1.0};
    double x{};
    double y{};
    double wx{};
    double wy{};
    double w{1.0};

    bool operator==(const StrokeTransform&) const = default;
};

struct StrokeTransformPoint {
    double x;
    double y;
};

[[nodiscard]] constexpr StrokeTransformPoint StrokeTransformApply(const StrokeTransform& transform, double x,
                                                                  double y) noexcept {
    const double w = (transform.wx * x) + (transform.wy * y) + transform.w;
    return StrokeTransformPoint{
        .x = ((transform.xx * x) + (transform.xy * y) + transform.x) / w,
        .y = ((transform.yx * x) + (transform.yy * y) + transform.y) / w,
    };
}

// A transform's lifted tiles, the grid positions under the selection's bounds, fit in the dab buffer of one frame as
// two pages each.
constexpr std::size_t kStrokeTransformTilesMax = std::size_t{kStrokeDabMax} * 4;
// Every texel the lasso covers tests all of its edges.
constexpr std::size_t kStrokeLassoPointsMax = 4096;
// DIPs between a lasso's points.
constexpr double kStrokeLassoSpacing = 2.0;

[[nodiscard]] constexpr SelectionBounds StrokeTransformBounds(const StrokeTransform& transform,
                                                              const SelectionBounds& box) noexcept {
    SelectionBounds bounds{
        .minX = std::numeric_limits<double>::max(),
        .minY = std::numeric_limits<double>::max(),
        .maxX = std::numeric_limits<double>::lowest(),
        .maxY = std::numeric_limits<double>::lowest(),
    };
    // With the divisor above 0 over the box, its image is the convex quad of its corners'.
    for (const double x : {box.minX, box.maxX}) {
        for (const double y : {box.minY, box.maxY}) {
            const StrokeTransformPoint to = StrokeTransformApply(transform, x, y);
            bounds.minX = std::min(bounds.minX, to.x);
            bounds.minY = std::min(bounds.minY, to.y);
            bounds.maxX = std::max(bounds.maxX, to.x);
            bounds.maxY = std::max(bounds.maxY, to.y);
        }
    }
    return bounds;
}

// Mirrors StrokeDab in stroke.cs.hlsl. A rectangle when `width` is above 0, from (x, y), its edge antialiased.
struct StrokeDab {
    float x;      // canvas units from the stroke's origin tile, small enough for float wherever the stroke is
    float y;
    float radius; // the brush's stamp()
    float width;
    float height;
    MdslDab inputs;
    std::array<float, kMdslParamFloatsMax> params; // the brush's, at MdslParam::offset, with their inputs applied
};
static_assert(sizeof(StrokeDab) == 104);

struct StrokePoint {
    double x;    // canvas units
    double y;
    double time; // seconds, PointerEvent's
    float pressure;
    float tiltX; // PointerEvent's
    float tiltY;
};

struct StrokeBeginInfo {
    BrushState brush;
    Color::Srgb color; // Paint: the paint. Erase: how much of each channel goes, white for all of them
    StrokePoint point;
    float smoothing{};
    float smoothingTail{1.0f};
    Layer layer;
    CanvasView view;
    StrokeMode mode;
    BlendMode blendMode; // Paint only
    StrokeShape shape;
    std::uint32_t shader{}; // ToolsState::shaders, whose stamp() places the dabs and whose pipeline paints them
};

struct StrokeTile {
    TileKey key;
    std::uint64_t touched;    // Morton bits of the 32^2 blocks any dab reached, archived as history
    std::uint32_t firstDab;   // the dabs before it miss the tile
    TileAtlasCell cell;       // kTileAtlasCellInvalid while waiting for the tile to stream in
    TileAtlasCell backupCell; // the tile before the stroke, kTileAtlasCellClear when the stroke created it;
                              // kTileAtlasCellInvalid when waiting or the atlas was full
    std::uint16_t dirtyMinX;  // texels to recompute, max exclusive, empty when min >= max
    std::uint16_t dirtyMinY;
    std::uint16_t dirtyMaxX;
    std::uint16_t dirtyMaxY;
    bool backedUp;
};

// The GPU recomputes every dirty texel from its tile's backup and all the dabs, so no temporary layer is
// needed, the opacity cap holds over existing paint and nothing accumulates in 8 bits.
struct StrokeState {
    std::vector<StrokeDab> dabs;
    std::vector<StrokeTile> tiles;
    BrushState brush;                             // as the stroke began
    std::array<float, kMdslCurveFloats> curves{}; // brush's, baked
    Color::Srgb color;
    StrokePoint first;                            // the first input point
    StrokePoint last;                             // the last point stepped to
    StrokePoint filtered;                         // the last input point after smoothing's low-pass filter
    StrokePoint tail;                             // the last raw input point
    std::array<StrokePoint, 3> knots{};           // the last Catmull-Rom knots, up to knotCount
    MdslDab inputs;                               // at `last`
    SelectionBounds selectionBefore;              // a stroke of the selection's mask: its canvas's bounds before it
    StrokeTransform transform;                    // Transform: where the pixels lifted from under selectionBefore go
    float toNextDab{};                            // canvas units along the path
    float smoothing{};                            // as StrokeBeginInfo's
    float smoothingTail{1.0f};                    // as StrokeBeginInfo's
    std::uint32_t seed{};                         // MdslDab::random's, a new one each stroke
    std::uint32_t stamps{};                       // placed, dropped ones included
    TileGridPos origin;            // the tile of the first point, the dabs and the GPU work relative to it
    Layer layer{};
    Canvas canvas{kCanvasInvalid}; // kCanvasInvalid: no stroke
    CanvasView view{kCanvasViewInvalid};
    StrokeMode mode{};
    BlendMode blendMode{};
    StrokeShape shape{};
    std::uint8_t knotCount{};
    std::uint32_t shader{};   // as StrokeBeginInfo's
    std::uint32_t endFrame{}; // gAppState.frame when it ended
    bool ended{};             // the GPU records what is left, then StrokeUpdate frees the backups
    bool discard{};           // ended having put every tile back: freed without a history record
};

// Whatever stamp() returns, a stamp stays within these, so a broken brush cannot touch every tile or place a stamp at
// every texel. Canvas units.
constexpr float kStrokeStampRadiusMax = 1024.0f;
constexpr float kStrokeStampSpacingMin = 0.25f;
constexpr float kStrokeStampOffsetMax = 1024.0f; // on each axis

struct StrokeStamp {
    std::array<float, kMdslParamFloatsMax> params; // as StrokeDab::params
    float radius;
    float spacing;
    float offsetX; // from the point on the path, canvas units
    float offsetY;
};

// Canvas units per second at which BrushInput::Speed reaches 1.
constexpr float kStrokeSpeedFull = 4000.0f;
// Seconds over which MdslDab::velocity follows the pen, so it does not jump with every input point.
constexpr double kStrokeVelocitySmoothing = 0.03;

// The stamp `brush` places with `inputs`: its params with their inputs applied, then `source`'s stamp() on them,
// reading `curves`, its curves baked. `brush` follows `source`'s params.
StrokeStamp StrokeStampAt(const MdslBrush& source, const BrushState& brush,
                          std::span<const float, kMdslCurveFloats> curves, const MdslDab& inputs) noexcept;

// Centripetal Catmull-Rom (alpha = 0.5) on [p1, p2] at `t` in 0..1, its tangents scaled by `tension` in 0..1 (0 is the
// straight segment from p1 to p2, 1 the full spline).
[[nodiscard]] inline StrokePoint StrokeCatmullRomAt(const StrokePoint& p0, const StrokePoint& p1, const StrokePoint& p2,
                                                    const StrokePoint& p3, double t, float tension = 1.0f) noexcept {
    const double u = std::clamp(t, 0.0, 1.0);
    const double dx = p2.x - p1.x;
    const double dy = p2.y - p1.y;
    const double d01 = std::sqrt(std::hypot(p1.x - p0.x, p1.y - p0.y));
    const double d12 = std::sqrt(std::hypot(dx, dy));
    const double d23 = std::sqrt(std::hypot(p3.x - p2.x, p3.y - p2.y));
    double m1x = dx;
    double m1y = dy;
    if (d01 > 1e-6 && d12 > 1e-6) {
        const double w0 = (d12 * d12) / (d01 * (d01 + d12));
        const double w1 = d01 / (d01 + d12);
        m1x = (w0 * (p1.x - p0.x)) + (w1 * dx);
        m1y = (w0 * (p1.y - p0.y)) + (w1 * dy);
    }
    double m2x = dx;
    double m2y = dy;
    if (d12 > 1e-6 && d23 > 1e-6) {
        const double w1 = d23 / (d12 + d23);
        const double w2 = (d12 * d12) / (d23 * (d12 + d23));
        m2x = (w1 * dx) + (w2 * (p3.x - p2.x));
        m2y = (w1 * dy) + (w2 * (p3.y - p2.y));
    }
    const double s = std::clamp(static_cast<double>(tension), 0.0, 1.0);
    const double t1x = std::lerp(dx, m1x, s);
    const double t1y = std::lerp(dy, m1y, s);
    const double t2x = std::lerp(dx, m2x, s);
    const double t2y = std::lerp(dy, m2y, s);
    const double u2 = u * u;
    const double u3 = u2 * u;
    const double h00 = (2.0 * u3) - (3.0 * u2) + 1.0;
    const double h10 = u3 - (2.0 * u2) + u;
    const double h01 = (-2.0 * u3) + (3.0 * u2);
    const double h11 = u3 - u2;
    const auto at = static_cast<float>(u);
    return StrokePoint{
        .x = (h00 * p1.x) + (h10 * t1x) + (h01 * p2.x) + (h11 * t2x),
        .y = (h00 * p1.y) + (h10 * t1y) + (h01 * p2.y) + (h11 * t2y),
        .time = std::lerp(p1.time, p2.time, u),
        .pressure = std::lerp(p1.pressure, p2.pressure, at),
        .tiltX = std::lerp(p1.tiltX, p2.tiltX, at),
        .tiltY = std::lerp(p1.tiltY, p2.tiltY, at),
    };
}

// Extrapolates the final spline segment [p1, p2] past p0 with zero curvature at p2 (m2 = 1.5 * (p2 - p1) - 0.5 * m1):
// position stops at `t * tail` along the curve, while time, pressure and tilt lerp across the full 0..1 `t` to `p2`.
[[nodiscard]] inline StrokePoint StrokeCatmullRomTailAt(const StrokePoint& p0, const StrokePoint& p1,
                                                        const StrokePoint& p2, double t, float tail,
                                                        float tension = 1.0f) noexcept {
    const double s = std::clamp(t, 0.0, 1.0);
    const double u = s * std::clamp(static_cast<double>(tail), 0.0, 1.0);
    const double dx = p2.x - p1.x;
    const double dy = p2.y - p1.y;
    const double d01 = std::sqrt(std::hypot(p1.x - p0.x, p1.y - p0.y));
    const double d12 = std::sqrt(std::hypot(dx, dy));
    double m1x = dx;
    double m1y = dy;
    if (d01 > 1e-6 && d12 > 1e-6) {
        const double w0 = (d12 * d12) / (d01 * (d01 + d12));
        const double w1 = d01 / (d01 + d12);
        m1x = (w0 * (p1.x - p0.x)) + (w1 * dx);
        m1y = (w0 * (p1.y - p0.y)) + (w1 * dy);
    }
    const double m2x = (1.5 * dx) - (0.5 * m1x);
    const double m2y = (1.5 * dy) - (0.5 * m1y);
    const double k = std::clamp(static_cast<double>(tension), 0.0, 1.0);
    const double t1x = std::lerp(dx, m1x, k);
    const double t1y = std::lerp(dy, m1y, k);
    const double t2x = std::lerp(dx, m2x, k);
    const double t2y = std::lerp(dy, m2y, k);
    const double u2 = u * u;
    const double u3 = u2 * u;
    const double h00 = (2.0 * u3) - (3.0 * u2) + 1.0;
    const double h10 = u3 - (2.0 * u2) + u;
    const double h01 = (-2.0 * u3) + (3.0 * u2);
    const double h11 = u3 - u2;
    const auto at = static_cast<float>(s);
    return StrokePoint{
        .x = (h00 * p1.x) + (h10 * t1x) + (h01 * p2.x) + (h11 * t2x),
        .y = (h00 * p1.y) + (h10 * t1y) + (h01 * p2.y) + (h11 * t2y),
        .time = std::lerp(p1.time, p2.time, s),
        .pressure = std::lerp(p1.pressure, p2.pressure, at),
        .tiltX = std::lerp(p1.tiltX, p2.tiltX, at),
        .tiltY = std::lerp(p1.tiltY, p2.tiltY, at),
    };
}

// The GPU has a backup or dirty texels to record.
[[nodiscard]] constexpr bool StrokeTilePending(const StrokeTile& tile) noexcept {
    return tile.backupCell != kTileAtlasCellInvalid && (!tile.backedUp || tile.dirtyMinX < tile.dirtyMaxX);
}

// The tile is in the stroke, or is a level 1-3 tile of one that is: those follow its paint, see RecordStroke.
[[nodiscard]] inline bool StrokeHasTile(const StrokeState& stroke, Canvas canvas, TileKey key) noexcept {
    return stroke.canvas == canvas
        && std::ranges::find(stroke.tiles, TileKeyWithMip(key, 0), &StrokeTile::key) != stroke.tiles.end();
}

// Edits the tiles of `layer` and deletes their level 1-3 tiles. Paint, Replace and Exclude create the tiles missing
// under the stroke while atlas cells can be taken; Erase and Intersect skip them. Replace and Intersect also take in
// every tile the layer has, so what no dab covers is cleared. A tile not streamed in waits for it and joins late. A
// Rect stroke has no dab until StrokeTo. A Transform takes in the tiles of `layer` and of the selection under the
// selection's bounds, and starts where they are.
void StrokeBegin(const StrokeBeginInfo& beginInfo) noexcept;
void StrokeTo(StrokePoint point) noexcept;
// A Transform's pixels go where `transform` puts them; where they were and where they go are recomputed, creating the
// tiles missing there.
void StrokeTransformTo(const StrokeTransform& transform) noexcept;
void StrokeEnd() noexcept;
// Once per update, after the frame's visible tiles are marked: attaches the waiting tiles that streamed in and
// keeps the others wanted. Once the GPU recorded all of an ended stroke, its backups become a history record and it
// is freed; what waiting tiles missed is dropped if they take too long.
void StrokeUpdate() noexcept;
// Frees the stroke now, whatever the GPU has not recorded yet is lost, and records no history.
void StrokeRelease() noexcept;

} // namespace Midori
