#pragma once

#include "midori/color.hpp"
#include "midori/mdsl.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Midori {

// What drives a brush param through a stroke, each from 0 to 1. Tilt maps its -1..1 lean to 0..1, speed reaches 1 at
// kStrokeSpeedFull, and random is MdslDab::random, a new one each stamp.
enum struct BrushInput : std::uint8_t {
    None,
    Pressure,
    TiltX,
    TiltY,
    Speed,
    Random,
    Count_,
};

// The value of one of the brush's MdslParams. With an input, `curve` maps the input's 0 to 1 to the share of the value
// the stamp gets.
struct BrushParam {
    Curve curve;
    std::array<float, 4> value{};
    BrushInput input{};
};

// What a tool's stamps are made with, whatever the tool does with them: the values of the brush's params and curves, in
// the order of its MdslParams and MdslCurves. Its stamp() and paint() say what they mean.
struct BrushState {
    std::array<BrushParam, kMdslParamFloatsMax> params{};
    std::array<Curve, kMdslCurvesMax> curves{};
};

// The defaults the brush's source declares.
inline void BrushParamsDefault(BrushState& brush, const MdslBrush& source) noexcept {
    for (std::size_t i = 0; i < source.params.size(); ++i) {
        brush.params[i] = BrushParam{.curve = {}, .value = source.params[i].value, .input = BrushInput::None};
    }
    for (std::size_t i = 0; i < source.curves.size(); ++i) {
        brush.curves[i] = source.curves[i].value;
    }
}

// What the GPU and stamp() read of the brush's curves.
inline void BrushCurvesBake(const BrushState& brush, std::span<float, kMdslCurveFloats> samples) noexcept {
    for (std::size_t i = 0; i < kMdslCurvesMax; ++i) {
        CurveBake(brush.curves[i], samples.subspan(i * kCurveSamples).first<kCurveSamples>());
    }
}

// Its index among `params`, or -1.
inline int BrushParamFind(std::span<const MdslParam> params, std::string_view name) noexcept {
    const auto found = std::ranges::find(params, name, &MdslParam::name);
    return found == params.end() ? -1 : static_cast<int>(found - params.begin());
}

inline int BrushCurveFind(std::span<const MdslCurve> curves, std::string_view name) noexcept {
    const auto found = std::ranges::find(curves, name, &MdslCurve::name);
    return found == curves.end() ? -1 : static_cast<int>(found - curves.begin());
}

enum struct Tool : std::uint8_t {
    Paint,
    Select, // a rectangle combined with the selection by ToolsState::selectionOp, or Shift, Alt, Shift+Alt
    Lasso,  // a freehand shape, combined the same way
};

// How a new selection combines with the one before it.
enum struct SelectionOp : std::uint8_t {
    Replace,
    Add,
    Subtract,
    Intersect,
    Exclude, // selected where exactly one of them is
    Count_,
};

// BlendMode::Erase erases with the brush, by `eraseColor` in place of `color`.
struct PaintToolState {
    BrushState brush;
    Color::Srgb color{.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f};
    // Times its alpha, how much of each channel goes: white erases everything and pure red only removes red.
    Color::Srgb eraseColor{.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f};
    BlendMode blendMode{};
    BlendMode paintBlendMode{}; // what tool.erase and tool.paint go back to from Erase
};

constexpr std::size_t kBrushPresetNameLenMax = 32;

struct BrushPreset {
    BrushState brush;
    std::array<char, kBrushPresetNameLenMax> name;
};

// brush.preset(number) copies presets[number - 1] into the Paint tool's brush.
struct ToolsState {
    std::vector<BrushPreset> presets;
    PaintToolState paint;
    Tool tool{};
    SelectionOp selectionOp{};
};

} // namespace Midori
