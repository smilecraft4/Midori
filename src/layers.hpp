#pragma once

#include "defines.hpp"
#include "colors.hpp"
#include <optional>

namespace Midori {

struct LayerCreateInfo {
    std::optional<Layer> id;
    std::string_view name{"Layer"};
    LayerHeight height{0};
    float opacity{1.0f};
    BlendMode blendMode{BlendMode::Alpha};
    bool hidden{false};
    bool mask{false};
    bool locked{false};
    bool internal{false};
};

struct LayerInfo {
    Layer id;
    LayerHeight height;
    float opacity;
    BlendMode blendMode;
    bool hidden;
    bool mask;
    bool locked;
    bool internal;
    bool modified;
};

struct LayerFileEntry {
    Layer id;
    LayerHeight height;
    float opacity;
    BlendMode blendMode;
    bool hidden;
    bool locked;
    bool mask;
    uint16_t nameLen;
    std::byte pad[2];
    // The remaining from here to here+nameLen are for the name (without terminator !!)
};

} // namespace Midori
