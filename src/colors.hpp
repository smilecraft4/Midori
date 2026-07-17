#pragma once

#include <cstdint>
#include <glm/vec4.hpp>

namespace Midori {

enum class BlendMode : uint8_t {
    Alpha,
    Multiply,
    Add,
};

using Color = glm::vec4;

}  // namespace Midori
