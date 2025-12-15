#ifndef MIDORI_TYPES_H
#define MIDORI_TYPES_H

#include <cstdint>
#include <glm/vec2.hpp>
#include <string>

#define UUID_SYSTEM_GENERATOR
#include "uuid.h"

namespace Midori {

enum class BlendMode : std::uint8_t {
    Normal,
    Multiply,
    Add,
    MAX,
};

using Layer = std::uint8_t;
constexpr size_t LAYER_MAX = UINT8_MAX;

struct LayerInfo {
    uuids::uuid id;
    std::string name = "Layer";
    float opacity = 1.0f;
    Layer layer = 0;
    std::uint8_t depth = 0;
    BlendMode blend_mode = BlendMode::Normal;
    bool visible = true;
    bool temporary = false;
    bool locked = false;
};

enum class TileError : uint8_t {
    None,
    Invalid,
    OutOfHandle,
    AlreadyExists,
    NotSaved,
    Missing,

    LayerMissing,

    MissingTexture,
    OutOfTextureUploadSlot,

    Unknwown,
};

using Tile = std::uint32_t;
constexpr size_t TILE_INVALID = 0;
constexpr size_t TILE_MAX = UINT32_MAX;
constexpr size_t TILE_SIZE = 256;

struct TileInfo {
    Layer layer;
    glm::ivec2 position;
};

}  // namespace Midori

#endif
