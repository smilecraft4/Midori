#pragma once

#include "defines.hpp"
#include "base.hpp"
#include <cstdint>
#include <type_traits>
#include <utility>

namespace Midori {

enum class TileState : uint8_t {
    None = 0,
    Loaded = 1 << 0,   // is loaded and can be showed
    Visible = 1 << 1,  // is visible
    Editable = 1 << 2, // can be edited using compute shaders
    Modified = 1 << 3, // is modified, needs to be downloaded when saving
};

constexpr TileState operator|(TileState lhs, TileState rhs) {
    return static_cast<TileState>(std::to_underlying(lhs) | std::to_underlying(rhs));
}

constexpr TileState& operator|=(TileState& lhs, TileState rhs) {
    lhs = lhs | rhs;
    return lhs;
}

enum class TileWriteState : uint8_t {
    Pending,
    Downloading,
    Encoding,
    Writting,
    Done,
};

enum class TileReadState : uint8_t {
    Pending,
    Read,
    Decoded,
    Uploaded,
    Done,
};

struct TileInfo {
    TileState state;
};

struct Tile {
    Layer layer{};
    TileAxis y{};
    TileAxis x{};
    uint8_t pad[2];

    bool operator==(const Tile& other) const noexcept {
        return layer == other.layer && y == other.y && x == other.x;
    }
};
static_assert(sizeof(Tile) == sizeof(size_t), "Tile coord must be 8bytes");

} // namespace Midori

template<>
struct std::hash<Midori::Tile> {
    size_t operator()(const Midori::Tile& t) const {
        size_t hash = static_cast<size_t>(std::make_unsigned_t<Layer>(t.layer)) << 48
                    | static_cast<size_t>(std::make_unsigned_t<TileAxis>(t.y)) << 32
                    | static_cast<size_t>(std::make_unsigned_t<TileAxis>(t.x)) << 16;
        return Splitmix64(hash);
    }
};
