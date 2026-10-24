#pragma once

#include "midori/color.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Midori {

constexpr float kBrushRadiusMin = 0.5f; // canvas units
constexpr float kBrushRadiusMax = 512.0f;

// Shape and dynamics of the dabs a tool draws with, whatever the tool does with them.
struct BrushState {
    float radius{16.0f};  // canvas units
    float hardness{0.9f}; // 0 fades from the center, 1 is a hard edge
    float flow{0.5f};     // alpha of one dab
    float opacity{1.0f};  // the most a stroke covers
    float spacing{0.1f};  // between dabs, in diameters
    // With pressure, no pressure to full pressure scale the property from min to max.
    float pressureRadiusMin{0.0f};
    float pressureRadiusMax{1.0f};
    float pressureOpacityMin{0.0f};
    float pressureOpacityMax{1.0f};
    float pressureFlowMin{0.0f};
    float pressureFlowMax{1.0f};
    float pressureHardnessMin{0.0f};
    float pressureHardnessMax{1.0f};
    bool pressureRadius{true};
    bool pressureOpacity{true};
    bool pressureFlow{false};
    bool pressureHardness{false};
};

enum struct Tool : std::uint8_t {
    Paint,
    Erase,
};

struct PaintToolState {
    BrushState brush;
    Color::Srgb color{.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f};
    BlendMode blendMode{};
};

struct EraseToolState {
    BrushState brush;
    // Times its alpha, how much of each channel goes. Alpha goes as far as all three channels do, so white
    // erases everything and pure red only removes red.
    Color::Srgb color{.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f};
};

constexpr std::size_t kBrushPresetNameLenMax = 32;

struct BrushPreset {
    BrushState brush;
    std::array<char, kBrushPresetNameLenMax> name;
};

// Each tool keeps its own brush. brush.preset(number) copies presets[number - 1] into the current tool's.
struct ToolsState {
    std::vector<BrushPreset> presets;
    PaintToolState paint;
    EraseToolState erase;
    Tool tool{};
};

[[nodiscard]] inline BrushState& ToolBrush(ToolsState& tools, Tool tool) noexcept {
    return tool == Tool::Erase ? tools.erase.brush : tools.paint.brush;
}

[[nodiscard]] inline const BrushState& ToolBrush(const ToolsState& tools, Tool tool) noexcept {
    return tool == Tool::Erase ? tools.erase.brush : tools.paint.brush;
}

} // namespace Midori
