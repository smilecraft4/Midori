#pragma once

#include "colors.h"
#include <string>
#define UUID_SYSTEM_GENERATOR
#include <uuid.h>

namespace Midori {

// Maybe this is too small. But I think the app will break before this number is achieved
using Layer = std::uint16_t;
using LayerHeight = std::uint16_t;

constexpr Layer LAYER_INVALID = UINT16_MAX;
constexpr Layer LAYERS_MAX = UINT16_MAX;

// TODO: use another method to identify layers then a uuid
// Always keep the default initialized values as the most common
struct LayerInfo {
    uuids::uuid id;      // unique id of the layer, across files etc..
    std::string name;    // Display name of the layer (can be the same as another layer)
    Layer layer;         // NOTE: Is this really necessary
    float transparency;  // How transparent is the window from : `0.0f` fully visible, `1.0f` fully transparent;
    LayerHeight height;  // Where the layer is compared to the other layers
    BlendMode blendMode; // Render mode for the layer
    bool folder;         // Does this layer stores other layers
    bool mask;           // Does this layer mask the output of the layer below
    bool hidden;         // Is this layer visible on the canvas
    bool locked;         // Is this layer modifiable (paintable, pasting, transforms)
    bool internal;       // Is this layer managed by the system and should not be editable/saved/tracked by the user
};

// TODO: try to match value in a cache friendly size
// static_assert(sizeof(LayerInfo) <= 8);
// static_assert(alignof(LayerInfo) <= 1);

} // namespace Midori
