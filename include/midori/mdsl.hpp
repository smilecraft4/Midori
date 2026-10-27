#pragma once

#include "midori/curve.hpp"
#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// mdsl, the Midori shading language: brushes written by users, checked here and translated in one pass. Params are
// globals the Brush window sets. Each stage takes its built-in input struct and returns its output struct, like HLSL's
// VSInput and PSOutput: stamp() runs on the CPU once per stamp, paint() on the GPU for every texel of every stamp.
//
//   param float size     = 16 range(0.5, 512);
//   param float hardness = 0.9 range(0, 1);
//
//   StampOutput stamp(StampInput input) {
//       StampOutput output;
//       output.radius = size;
//       output.spacing = 0.2 * size;
//       return output;
//   }
//
//   PaintOutput paint(PaintInput input) {
//       PaintOutput output;
//       float edge = max((1.0 - hardness) * input.radius, 1.0);
//       output.color = input.color.rgb;
//       output.amount = smoothstep(0.0, 1.0, (1.0 - length(input.local)) * input.radius / edge);
//       output.opacity = 1.0;
//       return output;
//   }
//
// StampInput: float pressure, vec2 tilt, vec2 velocity, vec2 direction, float random, float along, float time.
// PaintInput: the same, the stamp's, and vec2 local, float radius, vec4 color, vec2 position.
// StampOutput: float radius, float spacing, vec2 offset. PaintOutput: vec3 color, float amount, float opacity.
// An output starts at 0. A function of the brush's own can take an input struct; round.mdsl, in brushes/ next to the
// exe, says what each field is.
//
// Two more kinds of param are called like functions:
//
//   param curve falloff = curve(0, 0, 0.5, 0.8, 1, 1); // x, y pairs, from x 0 to x 1; linear by default
//   param texture grain = "paper.png";                // next to the brush file; none by default
//
// falloff(x) is the curve at x, component-wise, anywhere. grain(uv) is the texture's straight vec4 at uv, repeating
// every 1, paint() only; without a texture it is vec4(1).

namespace Midori {

// StrokeDab::params, every param's values one float after another.
constexpr std::uint32_t kMdslParamFloatsMax = 11;
// Mirror the Brush cbuffer and the texture table in stroke.cs.hlsl.
constexpr std::uint32_t kMdslCurvesMax = 4;
constexpr std::uint32_t kMdslTexturesMax = 4;
constexpr std::uint32_t kMdslCurveFloats = kMdslCurvesMax * kCurveSamples;

enum struct MdslType : std::uint8_t {
    Error,
    Void,
    Bool,
    Float,
    Vec2,
    Vec3,
    Vec4,
    StampInput, // what stamp() and paint() take and return
    PaintInput,
    StampOutput,
    PaintOutput,
    Curve,
    Texture,
};

constexpr std::uint32_t MdslComponents(MdslType type) noexcept {
    return type >= MdslType::Float && type <= MdslType::Vec4
             ? static_cast<std::uint32_t>(type) - static_cast<std::uint32_t>(MdslType::Float) + 1
             : 0;
}

// The UI builds a slider from it and writes its values into StrokeDab::params at `offset`.
struct MdslParam {
    std::string name;
    std::array<float, 4> value{}; // default
    float min{0.0f};
    float max{1.0f};
    std::uint32_t offset{};
    MdslType type{MdslType::Float};
};

struct MdslCurve {
    std::string name;
    Curve value; // default
};

struct MdslTexture {
    std::string name;
    std::string path; // default, as the source writes it: relative to the brush file; empty for none
};

// The interpreter's instructions. Every value is 4 floats; a float or bool fills all 4.
enum struct MdslOpCode : std::uint8_t {
    Const,     // push constants[arg]
    Input,     // push the input kept at arg
    Param,     // push `count` floats of the params from arg
    Load,      // push local arg
    Store,     // pop into local arg
    StoreMask, // pop into `count` components of local arg, picked by `pattern`
    Swizzle,   // `count` components of the top, picked by `pattern`
    Construct, // pop `count` values, whose sizes `pattern` holds, into one
    Call,      // functions[arg], its params popped into its first locals
    Return,
    Jump,      // to op arg
    JumpIfFalse,
    Neg,
    Not,
    Add,
    Sub,
    Mul,
    Div,
    Less,
    Greater,
    LessEq,
    GreaterEq,
    Equal,
    NotEqual,
    And,
    Or,
    Abs,
    Floor,
    Ceil,
    Fract,
    Sqrt,
    Exp,
    Log,
    Sin,
    Cos,
    Normalize, // of `count` components
    Pow,
    Min,
    Max,
    Step,
    Atan2,
    Clamp,
    Mix,
    Smoothstep,
    Length, // of `count` components
    Distance,
    Dot,
    Curve, // curve arg at each of `count` components
};

struct MdslOp {
    std::uint32_t arg;
    MdslOpCode code;
    std::uint8_t count;
    std::uint8_t pattern; // 2 bits per component, the first lowest
};

struct MdslFunction {
    std::uint32_t start;  // its first op
    std::uint32_t params; // its first locals
    std::uint32_t slots;  // locals
};

// stamp() and what it calls, checked so it fits the interpreter's fixed stack and locals.
struct MdslProgram {
    std::vector<MdslOp> ops;
    std::vector<std::array<float, 4>> constants;
    std::vector<MdslFunction> functions;
    std::uint32_t entry{}; // stamp()
};

struct MdslBrush {
    std::string hlsl;                  // defines `Paint BrushPaint(<paint()'s inputs in kInputs' order>,
                                       // float params[kMdslParamFloatsMax])`
    std::vector<MdslParam> params;
    std::vector<MdslCurve> curves;     // at most kMdslCurvesMax, BrushCurve's index
    std::vector<MdslTexture> textures; // at most kMdslTexturesMax, BrushTexture's index
    MdslProgram stamp;
};

// Shown to the user, in their source.
struct MdslError {
    std::string message;
    std::uint32_t line{}; // 1-based, 0 when the prelude is at fault
    std::uint32_t column{};
};

std::expected<MdslBrush, MdslError> MdslTranslate(std::string_view source) noexcept;

// What the engine measures at a stamp, the inputs stamp() and paint() share. StrokeDab carries it to the GPU.
struct MdslDab {
    float pressure;         // 0..1
    float tiltX;            // -1..1, 0 upright
    float tiltY;
    float velocityX;        // canvas units per second
    float velocityY;
    float directionX{1.0f}; // the path's, unit length
    float directionY;
    float random;           // 0..1
    float along;            // canvas units along the stroke
    float time;             // seconds since the stroke began
};

struct MdslStampInput {
    std::span<const float, kMdslParamFloatsMax> params; // as StrokeDab::params holds them
    std::span<const float, kMdslCurveFloats> curves;    // kCurveSamples of each, CurveBake's
    MdslDab dab;
};

struct MdslStamp {
    float radius;
    float spacing;
    float offsetX; // from the point on the path
    float offsetY;
};

// Runs stamp(). Whatever the brush computes, NaN and infinity included, comes back unchecked.
MdslStamp MdslStampRun(const MdslProgram& program, const MdslStampInput& input) noexcept;

} // namespace Midori
