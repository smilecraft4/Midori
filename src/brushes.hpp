#pragma once

#include <glm/vec4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec2.hpp>

namespace Midori {

// Used to configure how BrushDynamicData are applied to BrushData
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
};

// Data used to change the BrushData
struct PointerState {
    glm::vec3 pos;    // x, y, pressure
    glm::vec3 posVel; // dx, dy, dpressure
    glm::vec3 ang;    // tx, ty, tz
    glm::vec3 angVel; // dtx, dty, dtz
};

// Data used to modulate StrokePoint or other tings
struct BrushData {
    glm::vec4 color; // rgb + a
    float flow;
    float radius;
    float hardness;
    float spacing;

    BrushData(const BrushSettings& settings) {
        color.r = settings.color.r;
        color.g = settings.color.g;
        color.b = settings.color.b;
        color.a = settings.maxPressureOpacity;
        flow = settings.maxPressureFlow;
        radius = settings.maxPressureRadius;
        hardness = settings.maxPressureHardness;
        spacing = settings.maxPressureSpacing;
    }

    BrushData(const BrushSettings& settings, const PointerState& state) {
        color = settings.color;
        color.a *= std::lerp(settings.minPressureOpacity, settings.maxPressureOpacity, state.pos.z);
        flow = std::lerp(settings.minPressureFlow, settings.maxPressureFlow, state.pos.z);
        radius = std::lerp(settings.minPressureRadius, settings.maxPressureRadius, state.pos.z);
        hardness = std::lerp(settings.minPressureHardness, settings.maxPressureHardness, state.pos.z);
        spacing = std::lerp(settings.minPressureSpacing, settings.maxPressureSpacing, state.pos.z);
    }
};

}; // namespace Midori
