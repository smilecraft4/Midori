#pragma once

#include <glm/vec4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec2.hpp>

namespace Midori {

struct BrushSettings {
    glm::vec4 color{0.0f, 0.0f, 0.0f, 1.0f};
    float minPressureOpacity{0.0f};
    float maxPressureOpacity{1.0f};
    float minPressureRadius{0.0f};
    float maxPressureRadius{1.0f};
    float minPressureFlow{0.0f};
    float maxPressureFlow{1.0f};
    float minPressureHardness{0.0f};
    float maxPressureHardness{1.0f};
    float minPressureSpacing{1.0f};
    float maxPressureSpacing{1.0f};
    float smoothing{0.0f};
    bool modified;
};

struct PointerState {
    glm::vec3 pos;    // x, y, pressure
    glm::vec3 posVel; // dx, dy, dpressure
    glm::vec3 ang;    // tx, ty, tz
    glm::vec3 angVel; // dtx, dty, dtz
};

}; // namespace Midori
