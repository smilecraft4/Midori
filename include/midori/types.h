#ifndef MIDORI_TYPES_H
#define MIDORI_TYPES_H

#include <cstdint>
#include <glm/vec2.hpp>
#include <string>

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
  std::string name = "Layer";
  std::uint8_t depth = 0;

  // TODO: implement the rest of this
  BlendMode blend_mode = BlendMode::Normal;
  float transparency = 0.0f;
  bool internal = false;
  bool hidden = false;
  bool locked = false;
};

using Tile = std::uint32_t;
constexpr size_t TILE_MAX = UINT32_MAX;
constexpr size_t TILE_SIZE = 512;

struct TileInfo {
  Layer layer;
  glm::ivec2 position;
};

} // namespace Midori

#endif
