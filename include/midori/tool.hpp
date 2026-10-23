#pragma once

#include "midori/color.hpp"
#include <cstdint>

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
    bool pressureRadius{true};
    bool pressureOpacity{true};
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

// Each tool keeps its own brush.
struct ToolsState {
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
