#pragma once

#include "midori/canvas.hpp"
#include "midori/color.hpp"
#include "midori/tile.hpp"
#include "midori/tool.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace Midori {

constexpr std::uint32_t kStrokeDabMax = 1 << 16;

// Mirrors kStroke* in stroke.cs.hlsl.
enum struct StrokeMode : std::uint8_t {
    Paint,
    Erase,
};

// Mirrors StrokeDab in stroke.cs.hlsl.
struct StrokeDab {
    float x; // canvas units from the stroke's origin tile, small enough for float wherever the stroke is
    float y;
    float radius;
    float hardness;
    float flow;
    float opacity;
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
};

struct StrokeTile {
    TileKey key;
    std::uint32_t firstDab;   // the dabs before it miss the tile
    TileAtlasCell cell;       // kTileAtlasCellInvalid while waiting for the tile to stream in
    TileAtlasCell backupCell; // the tile before the stroke; kTileAtlasCellInvalid when waiting or the atlas was full
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
    BrushState brush;              // as the stroke began
    Color::Srgb color;
    StrokePoint last;              // the last input point
    float toNextDab{};             // canvas units along the path
    TileGridPos origin;            // the tile of the first point, the dabs and the GPU work relative to it
    Layer layer{};
    Canvas canvas{kCanvasInvalid}; // kCanvasInvalid: no stroke
    CanvasView view{kCanvasViewInvalid};
    StrokeMode mode{};
    BlendMode blendMode{};
    std::uint32_t endFrame{}; // gAppState.frame when it ended
    bool ended{};             // the GPU records what is left, then StrokeUpdate frees the backups
};

// The GPU has a backup or dirty texels to record.
[[nodiscard]] constexpr bool StrokeTilePending(const StrokeTile& tile) noexcept {
    return tile.backupCell != kTileAtlasCellInvalid && (!tile.backedUp || tile.dirtyMinX < tile.dirtyMaxX);
}

[[nodiscard]] inline bool StrokeHasTile(const StrokeState& stroke, Canvas canvas, TileKey key) noexcept {
    return stroke.canvas == canvas && std::ranges::find(stroke.tiles, key, &StrokeTile::key) != stroke.tiles.end();
}

// Edits the tiles of `layer` and deletes their level 1-3 tiles. Paint creates the tiles missing under the stroke
// while atlas cells can be taken; erase skips them. A tile not streamed in waits for it and joins late.
void StrokeBegin(const StrokeBeginInfo& beginInfo) noexcept;
void StrokeTo(StrokePoint point) noexcept;
void StrokeEnd() noexcept;
// Once per update, after the frame's visible tiles are marked: attaches the waiting tiles that streamed in and
// keeps the others wanted. Frees an ended stroke once the GPU recorded all of it; what waiting tiles missed is
// dropped if they take too long.
void StrokeUpdate() noexcept;
// Frees the stroke now, whatever the GPU has not recorded yet is lost.
void StrokeRelease() noexcept;

} // namespace Midori
