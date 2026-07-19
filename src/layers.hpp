#pragma once

#include "defines.hpp"
#include "colors.hpp"
#include "tiles.hpp"
#include <string>

namespace Midori {

struct LayerCreateInfo {
    std::string name{"Layer"};
    LayerHeight height{0};
    float opacity{1.0f};
    BlendMode blendMode{BlendMode::Alpha};
    bool hidden{false};
    bool internal{false};
};

struct LayerInfo {
    std::string name;
    Layer id;
    LayerHeight height;
    float opacity;
    BlendMode blendMode;
    bool hidden;
    bool internal;
    bool modified;
    hash_set<Tile> tiles; // To use later, currently we use the file system
};

} // namespace Midori
