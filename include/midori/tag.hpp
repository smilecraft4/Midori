#pragma once

#include "midori/canvas.hpp"
#include "midori/tool.hpp"
#include <cstdint>
#include <span>
#include <vector>

namespace Midori {

struct TagPoint {
    double x; // canvas units
    double y;
};

// A drag of the Tag tool, applied to its canvas's checked tags once released. Nothing is tagged while it lasts.
struct TagDragState {
    std::vector<TagPoint> points;        // a Box's first and last only
    CanvasView view{kCanvasViewInvalid}; // kCanvasViewInvalid: no drag
    ToolShape shape{};
    bool subtract{};                     // the tiles leave the tags rather than join them
};

// The grid positions of every tile the shape touches, sorted and unique. A Lasso of fewer than 3 points is its path.
[[nodiscard]] std::vector<TileKey> TagShapeTiles(ToolShape shape, std::span<const TagPoint> points) noexcept;
// The tags of `mask` gain `covered`, or lose it; then the islands are rebuilt.
void TagsApply(CanvasState& canvas, std::span<const TileKey> covered, bool subtract, std::uint64_t mask) noexcept;
// A new tag's hue, far from its neighbors'.
[[nodiscard]] float TagHueDefault(std::uint32_t tag) noexcept;
// Both tagIslands and tagSelectedIslands.
void TagIslandsBuild(CanvasState& canvas) noexcept;
void TagSelectedIslandsBuild(CanvasState& canvas) noexcept;

} // namespace Midori
