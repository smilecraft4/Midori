#pragma once

#include "layers.h"
#include <glm/vec2.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>
#include <EASTL/hash_map.h>
#include <EASTL/hash_set.h>

namespace Midori {

// This does not need to be this big because tiles are only assigned an ID when they are loaded. And I never expect to
// have UINT16_MAX tiles loaded at once
using Tile = uint16_t;

constexpr Tile TILE_INVALID = UINT16_MAX;
constexpr Tile TILES_MAX = UINT16_MAX; // Number of tile loaded at once
constexpr size_t TILE_WIDTH = 256;
constexpr size_t TILE_HEIGHT = 256;

struct TileCoord {
    Layer layer;
    glm::ivec2 pos;

    bool operator==(const TileCoord& other) const {
        return layer == other.layer && pos == other.pos;
    }
};


} // namespace Midori

namespace eastl {
    template <>
    struct hash<glm::ivec2> {
        size_t operator()(const glm::ivec2& v) const {
            return std::hash<glm::ivec2>{}(v);
        }
    };

    template <>
    struct hash<Midori::TileCoord> {
        size_t operator()(const Midori::TileCoord& tc) const {
            // this may create hash too big for what is trully needed
            size_t h1 = eastl::hash<Midori::Layer>{}(tc.layer);
            size_t h2 = eastl::hash<glm::ivec2>{}(tc.pos);
            return h1 ^ (h2 * 2654435761u);
        }
    };
}