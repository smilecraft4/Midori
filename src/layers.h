#pragma once

#include "colors.h"
#include <EASTL/unordered_map.h>
#include <EASTL/vector.h>
#include <string>

namespace Midori {

// Maybe this is too small. But I think the app will break before this number is achieved
using Layer = std::uint16_t;
using LayerHeight = std::uint16_t;

constexpr Layer LAYER_INVALID = UINT16_MAX;
constexpr Layer LAYERS_MAX = UINT16_MAX;

struct LayerInfo {
    std::string name;
    float opacity;
    Layer id{LAYER_INVALID}; // 0 is always the root layer of the canvas
    Layer parentId{0};
    LayerHeight height;
    BlendMode blendMode;
    bool folder;
    bool mask;
    bool hidden;
    bool locked;
    bool internal;
};

} // namespace Midori
