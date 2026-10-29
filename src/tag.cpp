#include "midori/tag.hpp"

#include "midori/tile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

#include <tracy/Tracy.hpp>

namespace Midori {

namespace {

constexpr auto kSize = static_cast<double>(kTileSize);

[[nodiscard]] TileKey GridKey(std::int64_t x, std::int64_t y) noexcept {
    return TileKeyMake(0, 0, TileGridPos{.x = static_cast<TileAxis>(x), .y = static_cast<TileAxis>(y)});
}

[[nodiscard]] std::int64_t TileOf(double v) noexcept {
    return static_cast<std::int64_t>(std::floor(v / kSize));
}

// Walks the grid from tile to tile along the segment, one axis step at a time.
void SegmentTiles(TagPoint a, TagPoint b, std::vector<TileKey>& tiles) noexcept {
    std::int64_t x = TileOf(a.x);
    std::int64_t y = TileOf(a.y);
    const std::int64_t endX = TileOf(b.x);
    const std::int64_t endY = TileOf(b.y);
    const double dx = (b.x - a.x) / kSize;
    const double dy = (b.y - a.y) / kSize;
    constexpr double kNever = std::numeric_limits<double>::infinity();
    const std::int64_t stepX = dx > 0.0 ? 1 : -1;
    const std::int64_t stepY = dy > 0.0 ? 1 : -1;
    const double deltaX = dx != 0.0 ? std::abs(1.0 / dx) : kNever;
    const double deltaY = dy != 0.0 ? std::abs(1.0 / dy) : kNever;
    const double startX = a.x / kSize;
    const double startY = a.y / kSize;
    double nextX = dx != 0.0
                     ? (stepX > 0 ? static_cast<double>(x + 1) - startX : startX - static_cast<double>(x)) * deltaX
                     : kNever;
    double nextY = dy != 0.0
                     ? (stepY > 0 ? static_cast<double>(y + 1) - startY : startY - static_cast<double>(y)) * deltaY
                     : kNever;
    tiles.push_back(GridKey(x, y));
    for (std::int64_t steps = std::abs(endX - x) + std::abs(endY - y); steps > 0; --steps) {
        if (nextX < nextY) {
            x += stepX;
            nextX += deltaX;
        } else {
            y += stepY;
            nextY += deltaY;
        }
        tiles.push_back(GridKey(x, y));
    }
}

// The tiles whose center the polygon holds by nonzero winding, a row of tile centers at a time.
void LassoInsideTiles(std::span<const TagPoint> points, std::vector<TileKey>& tiles) noexcept {
    const auto [minY, maxY] = std::ranges::minmax(points, {}, &TagPoint::y);
    struct Crossing {
        double x;
        int winding;
    };
    std::vector<Crossing> crossings;
    for (std::int64_t row = TileOf(minY.y); row <= TileOf(maxY.y); ++row) {
        const double centerY = (static_cast<double>(row) + 0.5) * kSize;
        crossings.clear();
        for (std::size_t i = 0; i < points.size(); ++i) {
            const TagPoint a = points[i];
            const TagPoint b = points[(i + 1) % points.size()];
            if ((a.y <= centerY) != (b.y <= centerY)) {
                crossings.push_back(
                    Crossing{.x = a.x + ((centerY - a.y) * (b.x - a.x) / (b.y - a.y)), .winding = b.y > a.y ? 1 : -1});
            }
        }
        std::ranges::sort(crossings, {}, &Crossing::x);
        int winding = 0;
        for (std::size_t i = 0; i + 1 < crossings.size(); ++i) {
            winding += crossings[i].winding;
            if (winding == 0) {
                continue;
            }
            const auto first = static_cast<std::int64_t>(std::ceil((crossings[i].x / kSize) - 0.5));
            const auto last = static_cast<std::int64_t>(std::floor((crossings[i + 1].x / kSize) - 0.5));
            for (std::int64_t column = first; column <= last; ++column) {
                tiles.push_back(GridKey(column, row));
            }
        }
    }
}

// The 8-connected places of the tiles carrying every tag of `mask`, in the order of their first tile.
void IslandsFind(std::span<const TagTile> tiles, std::uint64_t mask, std::uint32_t tag,
                 std::vector<TagIsland>& islands) noexcept {
    std::vector<TileKey> keys;
    for (const TagTile& tile : tiles) {
        if ((tile.tags & mask) == mask) {
            keys.push_back(tile.gridPos);
        }
    }
    std::vector<bool> seen(keys.size(), false);
    std::vector<std::size_t> pending;
    for (std::size_t start = 0; start < keys.size(); ++start) {
        if (seen[start]) {
            continue;
        }
        const TileGridPos first = TileKeyGridPos(keys[start]);
        TagIsland island{.minX = first.x, .minY = first.y, .maxX = first.x, .maxY = first.y, .tag = tag};
        seen[start] = true;
        pending.assign(1, start);
        while (!pending.empty()) {
            const TileGridPos pos = TileKeyGridPos(keys[pending.back()]);
            pending.pop_back();
            island.minX = std::min(island.minX, pos.x);
            island.minY = std::min(island.minY, pos.y);
            island.maxX = std::max(island.maxX, pos.x);
            island.maxY = std::max(island.maxY, pos.y);
            for (std::int64_t dy = -1; dy <= 1; ++dy) {
                for (std::int64_t dx = -1; dx <= 1; ++dx) {
                    const TileKey neighbor = GridKey(pos.x + dx, pos.y + dy);
                    const auto found = std::ranges::lower_bound(keys, neighbor);
                    if (found == keys.end() || *found != neighbor) {
                        continue;
                    }
                    const auto index = static_cast<std::size_t>(found - keys.begin());
                    if (!seen[index]) {
                        seen[index] = true;
                        pending.push_back(index);
                    }
                }
            }
        }
        islands.push_back(island);
    }
}

} // namespace

std::vector<TileKey> TagShapeTiles(ToolShape shape, std::span<const TagPoint> points) noexcept {
    ZoneScopedN("TagShapeTiles");
    std::vector<TileKey> tiles;
    if (points.empty()) {
        return tiles;
    }
    if (shape == ToolShape::Box) {
        const TagPoint a = points.front();
        const TagPoint b = points.back();
        // A tile only touched by the box's far edge is not in it.
        const std::int64_t minX = TileOf(std::min(a.x, b.x));
        const std::int64_t minY = TileOf(std::min(a.y, b.y));
        const std::int64_t maxX = std::max(minX, static_cast<std::int64_t>(std::ceil(std::max(a.x, b.x) / kSize)) - 1);
        const std::int64_t maxY = std::max(minY, static_cast<std::int64_t>(std::ceil(std::max(a.y, b.y) / kSize)) - 1);
        for (std::int64_t y = minY; y <= maxY; ++y) {
            for (std::int64_t x = minX; x <= maxX; ++x) {
                tiles.push_back(GridKey(x, y));
            }
        }
        return tiles;
    }
    SegmentTiles(points.front(), points.front(), tiles);
    for (std::size_t i = 1; i < points.size(); ++i) {
        SegmentTiles(points[i - 1], points[i], tiles);
    }
    if (shape == ToolShape::Lasso && points.size() >= 3) {
        SegmentTiles(points.back(), points.front(), tiles);
        LassoInsideTiles(points, tiles);
    }
    std::ranges::sort(tiles);
    const auto [first, last] = std::ranges::unique(tiles);
    tiles.erase(first, last);
    return tiles;
}

void TagsApply(CanvasState& canvas, std::span<const TileKey> covered, bool subtract, std::uint64_t mask) noexcept {
    ZoneScopedN("TagsApply");
    std::vector<TagTile> merged;
    merged.reserve(canvas.tagTiles.size() + covered.size());
    const auto keep = [&](TileKey gridPos, std::uint64_t tags, bool inside) {
        std::uint64_t combined = tags;
        if (inside) {
            combined = subtract ? tags & ~mask : tags | mask;
        }
        if (combined != 0) {
            merged.push_back(TagTile{.gridPos = gridPos, .tags = combined});
        }
    };
    std::size_t t = 0;
    std::size_t c = 0;
    while (t < canvas.tagTiles.size() || c < covered.size()) {
        const TagTile* tile = t < canvas.tagTiles.size() ? &canvas.tagTiles[t] : nullptr;
        if (c == covered.size() || (tile && tile->gridPos < covered[c])) {
            keep(tile->gridPos, tile->tags, false);
            ++t;
        } else if (!tile || covered[c] < tile->gridPos) {
            keep(covered[c], 0, true);
            ++c;
        } else {
            keep(tile->gridPos, tile->tags, true);
            ++t;
            ++c;
        }
    }
    canvas.tagTiles = std::move(merged);
    TagIslandsBuild(canvas);
}

float TagHueDefault(std::uint32_t tag) noexcept {
    return std::fmod(0.6f + (static_cast<float>(tag) * 0.618034f), 1.0f);
}

void TagIslandsBuild(CanvasState& canvas) noexcept {
    ZoneScopedN("TagIslandsBuild");
    canvas.tagIslands.clear();
    for (std::uint32_t tag = 0; tag < kTagMax; ++tag) {
        if ((canvas.tags & (std::uint64_t{1} << tag)) != 0) {
            IslandsFind(canvas.tagTiles, std::uint64_t{1} << tag, tag, canvas.tagIslands);
        }
    }
    TagSelectedIslandsBuild(canvas);
}

void TagSelectedIslandsBuild(CanvasState& canvas) noexcept {
    ZoneScopedN("TagSelectedIslandsBuild");
    canvas.tagsSelected &= canvas.tags;
    canvas.tagSelectedIslands.clear();
    if (canvas.tagsSelected != 0) {
        IslandsFind(canvas.tagTiles, canvas.tagsSelected, kTagMax, canvas.tagSelectedIslands);
    }
}

} // namespace Midori
