#pragma once

#include "midori/color.hpp"
#include "midori/mdsl.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
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

// A Layer's Box and Lasso are filled with the brush, the shape's edge its outer edge.
enum struct ToolShape : std::uint8_t {
    Path,  // stamps along the pointer's path
    Box,   // the rectangle from the first point to the last
    Lasso, // the freehand shape, closed back to the first point
};

// For now a Selection takes a Box or a Lasso only. Tags takes every shape, and the tiles it touches.
enum struct ToolTarget : std::uint8_t {
    Layer,
    Selection,
    Tags, // the canvas's checked tags
};

// How a new selection, or tagged area, combines with the one before it.
enum struct SelectionOp : std::uint8_t {
    Replace,
    Add,
    Subtract,
    Intersect,
    Exclude, // selected where exactly one of them is
    Count_,
};

constexpr std::size_t kToolNameLenMax = 32;
constexpr std::uint32_t kToolShadersMax = 32;

// The image one of a shader's textures reads, by its index in MdslBrush::textures. The shader's source names one, next
// to its file, unless one was picked in the Tool window.
struct BrushTextureState {
    std::filesystem::path path;                // empty: none, white
    std::filesystem::file_time_type writeTime; // `path`'s when last read
    std::string error;                         // why `path` gives no image
    bool chosen;                               // kept while the brush has a texture of the same name
};

// A .mdsl file tools paint with, by path. Translated when a tool first names it, so its tools' params read by name; its
// pipeline compiles when a tool using it is first picked. Kept until Midori exits; its index is its GraphicsBrush*
// handle.
struct ToolShader {
    std::filesystem::path path;                // empty: round.mdsl as built in, always index 0
    std::filesystem::file_time_type writeTime; // `path`'s when last read
    MdslBrush brush;                           // what its tools' params follow
    MdslBrush pending;                         // read again, its pipeline compiling or ready
    std::array<BrushTextureState, kMdslTexturesMax> textures;
    std::string error; // why it does not paint, or its last change does not: unreadable, the translator's or compiler's
    bool compiling;    // `pending`'s pipeline
    bool ready;        // compiled, waiting for the strokes using it to end
};

// One tool of the library. The toolbar places it by id, so editing it changes it everywhere. blendMode is for a Layer,
// where Erase erases everything the brush covers. selectionOp is for a Selection and Tags, unless a selection.op
// binding is held.
struct ToolState {
    BrushState brush;
    std::array<char, kToolNameLenMax> name{};
    std::uint32_t id{};     // stable, never 0: what tool.select(id) and the toolbar name
    std::uint32_t shader{}; // ToolsState::shaders
    BlendMode blendMode{};
    SelectionOp selectionOp{};
    ToolShape shape{};
    ToolTarget target{};
};

// A button for one tool, a drawer for more: pressing its key again while it is the active slot picks its next tool.
struct ToolbarSlot {
    std::vector<std::uint32_t> tools; // ToolState ids, never empty
    std::uint32_t id;                 // stable, never 0: what toolbar.slot(id) names
    std::uint32_t last;               // its tool picked last, which its button shows and picks
};

constexpr std::size_t kColorSlotsMin = 2;
constexpr std::size_t kColorSlotsMax = 8;
constexpr std::size_t kRecentColorsMax = 16;

// `library` is tools.json, never empty. `slots` is workspace.json, which only arranges the library's tools; the Tools
// panel lists the active slot's.
struct ToolsState {
    std::vector<ToolState> library;
    std::vector<ToolbarSlot> slots;
    std::vector<ToolShader> shaders; // the built-in one first, up to kToolShadersMax
    // kColorSlotsMin to kColorSlotsMax; every tool paints with colors[colorSlot].
    std::vector<Color::Srgb> colors{Color::Srgb{.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f}, Color::Srgb{.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f}};
    std::vector<Color::Srgb> recentColors; // painted with, the most recent first, up to kRecentColorsMax
    std::uint32_t colorSlot{};
    std::uint32_t current{}; // the tool painting or selecting
    std::uint32_t slot{};    // the active slot, 0 for none
};

ToolState* ToolFind(ToolsState& tools, std::uint32_t id) noexcept;
const ToolState* ToolFind(const ToolsState& tools, std::uint32_t id) noexcept;
// The library's first while `current` names none.
ToolState& ToolCurrent(ToolsState& tools) noexcept;
const ToolState& ToolCurrent(const ToolsState& tools) noexcept;
ToolbarSlot* ToolbarSlotFind(ToolsState& tools, std::uint32_t id) noexcept;
// The tool becomes the current one and its slot's last. When the active slot does not hold it, the first slot that does
// becomes the active one.
void ToolPick(ToolsState& tools, std::uint32_t id) noexcept;
// Picks the slot's last tool, or its next one, wrapping around, when it is already the active slot.
void ToolbarSlotPress(ToolsState& tools, std::uint32_t id) noexcept;
// A new slot at the end holding `tool`. Returns its id.
std::uint32_t ToolbarSlotAdd(ToolsState& tools, std::uint32_t tool) noexcept;
// A copy right after the tool in the library, and in the active slot when that holds it. Returns its id, 0 for none.
std::uint32_t ToolDuplicate(ToolsState& tools, std::uint32_t id) noexcept;
// Out of the library and every slot. The last tool stays.
void ToolDelete(ToolsState& tools, std::uint32_t id) noexcept;
// Slots keep only the library's tools, and go when none is left; `current`, `slot` and each `last` name what exists.
// The library must not be empty.
void ToolsFix(ToolsState& tools) noexcept;
// What the tool's params follow: its shader's brush.
const MdslBrush& ToolSource(const ToolsState& tools, const ToolState& tool) noexcept;
// `path`'s source translated, or nothing with `error` saying why: unreadable, or the translator's at line:col.
std::optional<MdslBrush> ToolShaderRead(const std::filesystem::path& path, std::string& error) noexcept;
// The shader at `path`, read and added when none has it yet; the built-in one for an empty path, kToolShadersMax when
// they are full. One that does not translate keeps its error and the built-in brush until it does.
std::uint32_t ToolShaderAdd(ToolsState& tools, const std::filesystem::path& path) noexcept;

} // namespace Midori
