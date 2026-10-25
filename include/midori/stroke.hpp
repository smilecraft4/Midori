#pragma once

#include "midori/canvas.hpp"
#include "midori/color.hpp"
#include "midori/tile.hpp"
#include "midori/tool.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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
    Transform, // no dab: the selected pixels of the layer and the selection with them, placed by StrokeState::transform
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
    float x; // canvas units from the stroke's origin tile, small enough for float wherever the stroke is
    float y;
    float radius;
    float hardness;
    float flow;
    float opacity;
    float width;
    float height;
};

struct StrokePoint {
    double x; // canvas units
    double y;
    float pressure;
};

struct StrokeBeginInfo {
    BrushState brush;
    Color::Srgb color; // Paint: the paint. Erase: how much of each channel goes, see EraseToolState
    StrokePoint point;
    Layer layer;
    CanvasView view;
    StrokeMode mode;
    BlendMode blendMode; // Paint only
    StrokeShape shape;
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
    BrushState brush;                // as the stroke began
    Color::Srgb color;
    StrokePoint first;               // the first input point
    StrokePoint last;                // the last input point
    SelectionBounds selectionBefore; // a stroke of the selection's mask: its canvas's bounds before it
    StrokeTransform transform;       // Transform: where the pixels lifted from under selectionBefore go
    float toNextDab{};               // canvas units along the path
    TileGridPos origin;              // the tile of the first point, the dabs and the GPU work relative to it
    Layer layer{};
    Canvas canvas{kCanvasInvalid};   // kCanvasInvalid: no stroke
    CanvasView view{kCanvasViewInvalid};
    StrokeMode mode{};
    BlendMode blendMode{};
    StrokeShape shape{};
    std::uint32_t endFrame{}; // gAppState.frame when it ended
    bool ended{};             // the GPU records what is left, then StrokeUpdate frees the backups
    bool discard{};           // ended having put every tile back: freed without a history record
};

[[nodiscard]] constexpr float DabRadius(const BrushState& brush, float pressure) noexcept {
    return brush.pressureRadius ? brush.radius * std::lerp(brush.pressureRadiusMin, brush.pressureRadiusMax, pressure)
                                : brush.radius;
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
