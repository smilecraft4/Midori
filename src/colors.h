#pragma once

#include <cstdint>

namespace Midori {

enum class BlendMode : uint8_t {
    Alpha,
    Multiply,
    Add,
};

struct Color {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};
}  // namespace Midori
