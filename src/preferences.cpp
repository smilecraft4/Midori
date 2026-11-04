#include "midori/preferences.hpp"

#include "midori/log.hpp"
#include "midori/platform.hpp"
#include "midori/text.hpp"
#include "midori/version.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <json.hpp>
#include <tracy/Tracy.hpp>

namespace Midori {

namespace {

using Json = nlohmann::json;

// Bumped when a value changes meaning, so a later version can read an older file.
constexpr int kPreferencesVersion = 2;
// Names rather than numbers, so the file reads and edits by hand. Indexed by the enums they name.
constexpr std::array<std::string_view, std::to_underlying(BlendMode::Count_)> kBlendModeNames = {
    "normal", "multiply", "add", "screen", "normalOkLab", "erase",
};
constexpr std::array<std::string_view, 3> kToolShapeNames = {"path", "box", "lasso"};
constexpr std::array<std::string_view, 4> kToolTargetNames = {"layer", "selection", "tags", "color"};
constexpr std::array<std::string_view, std::to_underlying(EyedropperSource::Count_)> kEyedropperSourceNames = {
    "canvas",
    "layer",
};
constexpr std::array<std::string_view, std::to_underlying(SelectionOp::Count_)> kSelectionOpNames = {
    "replace", "add", "subtract", "intersect", "exclude",
};
// Bumped like kPreferencesVersion, for tools.json and workspace.json.
constexpr int kToolsVersion = 1;
constexpr std::array<std::string_view, kBrushInputCount> kBrushInputNames = {
    "pressure", "tiltX", "tiltY", "speed", "random",
};
constexpr std::array<std::string_view, std::to_underlying(CanvasFilter::Count_)> kCanvasFilterNames = {
    "nearest", "bilinear", "bicubic", "mitchell", "lanczos2", "lanczos3",
};
// The display modes a window opens with; minimized and hidden are not kept.
constexpr std::array<std::string_view, 3> kDisplayModeNames = {"normal", "maximized", "fullscreen"};
constexpr std::array kDisplayModes = {WindowDisplayMode::Normal, WindowDisplayMode::Maximized,
                                      WindowDisplayMode::Fullscreened};
// A client area from a speck to past any screen, in DIPs.
constexpr int kWindowSizeMin = 64;
constexpr int kWindowSizeMax = 32768;

// A file of the app's folder under PlatformUserSettingsPath.
Result<std::filesystem::path> SettingsPath(std::string_view name) noexcept {
    const auto settings = PlatformUserSettingsPath();
    if (!settings) {
        return std::unexpected(settings.error());
    }
    return *settings / MIDORI_NAME / name;
}

// Null when the file is missing. One that is not JSON is set aside as <file>.bad, so the next write does not lose it.
Json JsonFileRead(const std::filesystem::path& path) noexcept {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Json();
    }
    Json root = Json::parse(file, nullptr, false);
    file.close();
    if (root.is_discarded()) {
        std::filesystem::path bad = path;
        bad += ".bad";
        std::error_code ec;
        std::filesystem::rename(path, bad, ec);
        MIDORI_LOG_WARN("{} is not valid JSON, set aside as {}", PathUtf8(path), PathUtf8(bad));
        return Json();
    }
    return root;
}

// Written aside then renamed over the file, so a failed write leaves the last one whole. A file name not valid UTF-16
// would make invalid UTF-8; replaced rather than failing.
Result<void> JsonFileWrite(const std::filesystem::path& path, const Json& root) noexcept {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        MIDORI_LOG_ERROR("creating {} failed: {}", PathUtf8(path.parent_path()), ec.message());
        return std::unexpected(Error::PreferencesFailed);
    }
    const std::string text = root.dump(4, ' ', false, Json::error_handler_t::replace);
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) {
            MIDORI_LOG_ERROR("writing {} failed", PathUtf8(temp));
            return std::unexpected(Error::PreferencesFailed);
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        MIDORI_LOG_ERROR("replacing {} failed: {}", PathUtf8(path), ec.message());
        std::filesystem::remove(temp, ec);
        return std::unexpected(Error::PreferencesFailed);
    }
    return {};
}

// Each Read leaves `value` as it is unless `json[key]` holds the right kind of value. Nothing here throws: the file is
// the user's, so every value is checked before it is read.
void Read(const Json& json, const char* key, float& value, float min, float max) noexcept {
    if (const auto found = json.find(key); found != json.end() && found->is_number()) {
        value = std::clamp(found->get<float>(), min, max);
    }
}

void Read(const Json& json, const char* key, int& value, int min, int max) noexcept {
    if (const auto found = json.find(key); found != json.end() && found->is_number_integer()) {
        value = static_cast<int>(std::clamp(found->get<std::int64_t>(), std::int64_t{min}, std::int64_t{max}));
    }
}

void Read(const Json& json, const char* key, std::uint32_t& value) noexcept {
    if (const auto found = json.find(key);
        found != json.end() && found->is_number_unsigned()
        && found->get<std::uint64_t>() <= std::numeric_limits<std::uint32_t>::max()) {
        value = static_cast<std::uint32_t>(found->get<std::uint64_t>());
    }
}

void Read(const Json& json, const char* key, bool& value) noexcept {
    if (const auto found = json.find(key); found != json.end() && found->is_boolean()) {
        value = found->get<bool>();
    }
}

// [r, g, b, a], each from 0 to 1.
[[nodiscard]] bool ColorRead(const Json& json, Color::Srgb& color) noexcept {
    if (!json.is_array() || json.size() != 4
        || !std::ranges::all_of(json, [](const Json& channel) { return channel.is_number(); })) {
        return false;
    }
    color = Color::Srgb{
        .r = std::clamp(json[0].get<float>(), 0.0f, 1.0f),
        .g = std::clamp(json[1].get<float>(), 0.0f, 1.0f),
        .b = std::clamp(json[2].get<float>(), 0.0f, 1.0f),
        .a = std::clamp(json[3].get<float>(), 0.0f, 1.0f),
    };
    return true;
}

void Read(const Json& json, const char* key, Color::Srgb& color) noexcept {
    if (const auto found = json.find(key); found != json.end()) {
        static_cast<void>(ColorRead(*found, color));
    }
}

// Kept only with at least `min` valid colors; the invalid ones are skipped, the ones past `max` dropped.
void Read(const Json& json, const char* key, std::vector<Color::Srgb>& colors, std::size_t min,
          std::size_t max) noexcept {
    const auto found = json.find(key);
    if (found == json.end() || !found->is_array()) {
        return;
    }
    std::vector<Color::Srgb> read;
    for (const Json& entry : *found) {
        if (Color::Srgb color; read.size() < max && ColorRead(entry, color)) {
            read.push_back(color);
        }
    }
    if (read.size() >= min) {
        colors = std::move(read);
    }
}

// The index of the name `json[key]` holds among `names`, -1 for none.
[[nodiscard]] int ReadName(const Json& json, const char* key, std::span<const std::string_view> names) noexcept {
    const auto found = json.find(key);
    if (found == json.end() || !found->is_string()) {
        return -1;
    }
    const auto name = std::ranges::find(names, found->get_ref<const std::string&>());
    return name == names.end() ? -1 : static_cast<int>(name - names.begin());
}

[[nodiscard]] std::filesystem::path PathFromUtf8(const std::string& utf8) noexcept {
    return std::u8string(utf8.begin(), utf8.end());
}

// UTF-8 strings, the first `max` of them.
void Read(const Json& json, const char* key, std::vector<std::filesystem::path>& paths, std::size_t max) noexcept {
    const auto found = json.find(key);
    if (found == json.end() || !found->is_array()) {
        return;
    }
    for (const Json& entry : *found) {
        if (paths.size() == max) {
            break;
        }
        if (entry.is_string()) {
            paths.push_back(PathFromUtf8(entry.get_ref<const std::string&>()));
        }
    }
}

[[nodiscard]] Json PathsJson(std::span<const std::filesystem::path> paths) noexcept {
    Json json = Json::array();
    for (const std::filesystem::path& path : paths) {
        json.push_back(PathUtf8(path));
    }
    return json;
}

// [[x, y], ...], kept only when it makes a valid curve.
void Read(const Json& json, const char* key, Curve& curve) noexcept {
    const auto found = json.find(key);
    if (found == json.end() || !found->is_array() || found->size() < 2 || found->size() > kCurvePointsMax) {
        return;
    }
    Curve read{};
    read.count = static_cast<std::uint32_t>(found->size());
    for (std::uint32_t i = 0; i < read.count; ++i) {
        const Json& point = (*found)[i];
        if (!point.is_array() || point.size() != 2 || !point[0].is_number() || !point[1].is_number()) {
            return;
        }
        read.points[i] = CurvePoint{.x = point[0].get<float>(), .y = point[1].get<float>()};
    }
    if (CurveValid(read)) {
        curve = read;
    }
}

[[nodiscard]] Json CurveJson(const Curve& curve) noexcept {
    Json points = Json::array();
    for (std::uint32_t i = 0; i < curve.count; ++i) {
        points.push_back(Json::array({curve.points[i].x, curve.points[i].y}));
    }
    return points;
}

// By name, so a param keeps its value when the brush's source moves it. A value takes as many numbers as the param has
// components, each within the param's range.
void ReadBrushParams(const Json& json, BrushState& brush, std::span<const MdslParam> params) noexcept {
    const auto found = json.find("params");
    if (found == json.end() || !found->is_object()) {
        return;
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
        const auto entry = found->find(params[i].name);
        if (entry == found->end() || !entry->is_object()) {
            continue;
        }
        BrushParam& param = brush.params[i];
        const std::uint32_t components = MdslComponents(params[i].type);
        if (const auto value = entry->find("value");
            value != entry->end() && value->is_array() && value->size() == components
            && std::ranges::all_of(*value, [](const Json& number) { return number.is_number(); })) {
            for (std::uint32_t c = 0; c < components; ++c) {
                param.value[c] = std::clamp((*value)[c].get<float>(), params[i].min, params[i].max);
            }
        }
        if (const auto inputs = entry->find("inputs"); inputs != entry->end() && inputs->is_object()) {
            for (std::size_t input = 0; input < kBrushInputCount; ++input) {
                const char* inputName = kBrushInputNames[input].data();
                if (inputs->contains(inputName)) {
                    param.inputs |= static_cast<std::uint8_t>(1u << input);
                    Read(*inputs, inputName, param.curves[input]);
                }
            }
            continue;
        }
        // Before a param took several inputs: one "input" with its "curve", and before curves, a min and max.
        const int input = ReadName(*entry, "input", kBrushInputNames);
        if (input < 0) {
            continue;
        }
        Curve& curve = param.curves[static_cast<std::size_t>(input)];
        param.inputs = static_cast<std::uint8_t>(1u << input);
        Read(*entry, "curve", curve);
        float min = 0.0f;
        float max = 1.0f;
        Read(*entry, "min", min, 0.0f, 1.0f);
        Read(*entry, "max", max, min, 1.0f);
        for (std::uint32_t p = 0; p < curve.count; ++p) {
            curve.points[p].y = std::lerp(min, max, curve.points[p].y);
        }
    }
}

// Starts from the defaults of the brush's source.
void ReadBrush(const Json& json, BrushState& brush, const MdslBrush& source) noexcept {
    BrushParamsDefault(brush, source);
    const auto found = json.find("brush");
    if (found == json.end() || !found->is_object()) {
        return;
    }
    ReadBrushParams(*found, brush, source.params);
    if (const auto curves = found->find("curves"); curves != found->end() && curves->is_object()) {
        for (std::size_t i = 0; i < source.curves.size(); ++i) {
            Read(*curves, source.curves[i].name.c_str(), brush.curves[i]);
        }
    }
}

// Of kDisplayModeNames: minimized and hidden reopen normal.
[[nodiscard]] std::string_view DisplayModeName(WindowDisplayMode displayMode) noexcept {
    switch (displayMode) {
        case WindowDisplayMode::Maximized:
            return kDisplayModeNames[1];
        case WindowDisplayMode::Fullscreened:
            return kDisplayModeNames[2];
        case WindowDisplayMode::Normal:
        case WindowDisplayMode::Hidden:
        case WindowDisplayMode::Minimized:
            break;
    }
    return kDisplayModeNames[0];
}

[[nodiscard]] Json BrushJson(const BrushState& brush, const MdslBrush& source) noexcept {
    const std::span<const MdslParam> params = source.params;
    Json paramsJson = Json::object();
    for (std::size_t i = 0; i < params.size(); ++i) {
        const BrushParam& param = brush.params[i];
        Json inputsJson = Json::object();
        for (std::size_t input = 0; input < kBrushInputCount; ++input) {
            if (BrushParamUses(param, static_cast<BrushInput>(input))) {
                inputsJson[std::string(kBrushInputNames[input])] = CurveJson(param.curves[input]);
            }
        }
        paramsJson[params[i].name] = Json{
            {"value", std::vector<float>(param.value.begin(), param.value.begin() + MdslComponents(params[i].type))},
            {"inputs", inputsJson},
        };
    }
    Json curvesJson = Json::object();
    for (std::size_t i = 0; i < source.curves.size(); ++i) {
        curvesJson[source.curves[i].name] = CurveJson(brush.curves[i]);
    }
    return Json{{"params", paramsJson}, {"curves", curvesJson}};
}

[[nodiscard]] Json ColorJson(const Color::Srgb& color) noexcept {
    return Json::array({color.r, color.g, color.b, color.a});
}

[[nodiscard]] Json ColorsJson(std::span<const Color::Srgb> colors) noexcept {
    Json json = Json::array();
    for (const Color::Srgb& color : colors) {
        json.push_back(ColorJson(color));
    }
    return json;
}

// The brushes folder next to Midori, which tools.json names shaders from.
std::filesystem::path BrushesFolder() noexcept {
    const auto folder = PlatformExecutableFolder();
    return folder ? *folder / "brushes" : std::filesystem::path();
}

// brushes/round.mdsl, so editing it shows at once; the built-in one when it is missing.
std::filesystem::path ShaderDefault() noexcept {
    std::filesystem::path path = BrushesFolder() / "round.mdsl";
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) ? path : std::filesystem::path();
}

// Relative to the brushes folder when inside it, empty for the built-in shader.
std::string ShaderPathWrite(const std::filesystem::path& path) noexcept {
    if (path.empty()) {
        return {};
    }
    const std::filesystem::path relative = path.lexically_relative(BrushesFolder());
    if (!relative.empty() && *relative.begin() != "..") {
        return PathUtf8(relative);
    }
    return PathUtf8(path);
}

// The tool's shader, ShaderDefault without one, the built-in one when the shaders are full.
std::uint32_t ReadShader(const Json& json, ToolsState& tools) noexcept {
    std::filesystem::path path = ShaderDefault();
    if (const auto found = json.find("shader"); found != json.end() && found->is_string()) {
        const std::string& utf8 = found->get_ref<const std::string&>();
        path = std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
        if (!path.empty() && path.is_relative()) {
            path = BrushesFolder() / path;
        }
    }
    const std::uint32_t shader = ToolShaderAdd(tools, path);
    return shader < kToolShadersMax ? shader : 0;
}

// The defaults of the tool's shader for what `json` misses; id 0 when it has none.
ToolState ReadTool(const Json& json, ToolsState& tools) noexcept {
    ToolState tool{};
    tool.shader = ReadShader(json, tools);
    ReadBrush(json, tool.brush, ToolSource(tools, tool));
    Read(json, "id", tool.id);
    if (const auto name = json.find("name"); name != json.end() && name->is_string()) {
        const std::string& text = name->get_ref<const std::string&>();
        std::copy_n(text.begin(), std::min(text.size(), tool.name.size() - 1), tool.name.begin());
    }
    if (const int blendMode = ReadName(json, "blendMode", kBlendModeNames); blendMode >= 0) {
        tool.blendMode = static_cast<BlendMode>(blendMode);
    }
    if (const int op = ReadName(json, "selectionOp", kSelectionOpNames); op >= 0) {
        tool.selectionOp = static_cast<SelectionOp>(op);
    }
    if (const int target = ReadName(json, "target", kToolTargetNames); target >= 0) {
        tool.target = static_cast<ToolTarget>(target);
    }
    if (const int shape = ReadName(json, "shape", kToolShapeNames); shape >= 0) {
        tool.shape = static_cast<ToolShape>(shape);
    }
    // For now a Selection takes a Box or a Lasso only.
    if (tool.target == ToolTarget::Selection && tool.shape == ToolShape::Path) {
        tool.shape = ToolShape::Box;
    }
    return tool;
}

Json ToolJson(const ToolState& tool, const ToolsState& tools) noexcept {
    return Json{
        {"id", tool.id},
        {"name", tool.name.data()},
        {"target", kToolTargetNames[std::to_underlying(tool.target)]},
        {"shape", kToolShapeNames[std::to_underlying(tool.shape)]},
        {"blendMode", kBlendModeNames[std::to_underlying(tool.blendMode)]},
        {"selectionOp", kSelectionOpNames[std::to_underlying(tool.selectionOp)]},
        {"shader", ShaderPathWrite(tools.shaders[tool.shader].path)},
        {"brush", BrushJson(tool.brush, ToolSource(tools, tool))},
    };
}

// With ShaderDefault.
ToolState ToolMake(std::uint32_t id, std::string_view name, ToolTarget target, ToolShape shape, BlendMode blendMode,
                   ToolsState& tools) noexcept {
    ToolState tool{.id = id, .blendMode = blendMode, .shape = shape, .target = target};
    tool.shader = ToolShaderAdd(tools, ShaderDefault());
    tool.shader = tool.shader < kToolShadersMax ? tool.shader : 0;
    BrushParamsDefault(tool.brush, ToolSource(tools, tool));
    std::copy_n(name.begin(), std::min(name.size(), tool.name.size() - 1), tool.name.begin());
    return tool;
}

// A Pen, an Eraser, a drawer of Rectangle and Lasso, a Tag and an Eyedropper, which kDefaultBindings key B, E, M and
// I. An older preferences.json's paint brush goes to the Pen, its brush presets into the Pen's drawer.
void ToolsDefault(ToolsState& tools, const Json& preferences) noexcept {
    tools.library.push_back(ToolMake(1, "Pen", ToolTarget::Layer, ToolShape::Path, BlendMode::Normal, tools));
    tools.library.push_back(ToolMake(2, "Eraser", ToolTarget::Layer, ToolShape::Path, BlendMode::Erase, tools));
    tools.library.push_back(ToolMake(3, "Rectangle", ToolTarget::Selection, ToolShape::Box, BlendMode::Normal, tools));
    tools.library.push_back(ToolMake(4, "Lasso", ToolTarget::Selection, ToolShape::Lasso, BlendMode::Normal, tools));
    tools.library.push_back(ToolMake(5, "Tag", ToolTarget::Tags, ToolShape::Box, BlendMode::Normal, tools));
    tools.library.push_back(ToolMake(6, "Eyedropper", ToolTarget::Color, ToolShape::Path, BlendMode::Normal, tools));
    tools.slots = {
        ToolbarSlot{.tools = {1}, .id = 1, .last = 1},    ToolbarSlot{.tools = {2}, .id = 2, .last = 2},
        ToolbarSlot{.tools = {3, 4}, .id = 3, .last = 3}, ToolbarSlot{.tools = {5}, .id = 4, .last = 5},
        ToolbarSlot{.tools = {6}, .id = 5, .last = 6},
    };
    tools.current = 1;
    tools.slot = 1;
    if (!preferences.is_object()) {
        return;
    }
    for (const char* key : {"tool", "paint"}) {
        if (const auto paint = preferences.find(key); paint != preferences.end() && paint->is_object()) {
            ReadBrush(*paint, tools.library[0].brush, ToolSource(tools, tools.library[0]));
            break;
        }
    }
    if (const auto presets = preferences.find("brushPresets"); presets != preferences.end() && presets->is_array()) {
        for (const Json& entry : *presets) {
            if (!entry.is_object()) {
                continue;
            }
            ToolState tool = ReadTool(entry, tools);
            tool.id = static_cast<std::uint32_t>(tools.library.size()) + 1;
            tools.library.push_back(tool);
            tools.slots[0].tools.push_back(tool.id);
        }
    }
}

} // namespace

Result<Preferences> PreferencesLoad() noexcept {
    ZoneScopedN("PreferencesLoad");
    Preferences preferences{};
    const auto path = SettingsPath("preferences.json");
    if (!path) {
        return std::unexpected(path.error());
    }
    std::ifstream file(*path, std::ios::binary);
    if (!file) {
        return preferences;
    }
    const Json root = Json::parse(file, nullptr, false);
    file.close();
    if (root.is_discarded() || !root.is_object()) {
        std::filesystem::path bad = *path;
        bad += ".bad";
        std::error_code ec;
        std::filesystem::rename(*path, bad, ec);
        MIDORI_LOG_WARN("{} is not valid JSON, set aside as {}", PathUtf8(*path), PathUtf8(bad));
        return std::unexpected(Error::PreferencesFailed);
    }

    if (const auto language = root.find("language");
        language != root.end() && language->is_string()
        && std::ranges::contains(kLanguages, language->get_ref<const std::string&>(), &LanguageInfo::code)) {
        preferences.language = language->get_ref<const std::string&>();
    }
    if (const int filter = ReadName(root, "canvasFilter", kCanvasFilterNames); filter >= 0) {
        preferences.canvasFilter = static_cast<CanvasFilter>(filter);
    }
    Read(root, "zoomMin", preferences.zoomMin, kViewportZoomMin, kViewportZoomMax);
    Read(root, "zoomMax", preferences.zoomMax, kViewportZoomMin, kViewportZoomMax);
    preferences.zoomMax = std::max(preferences.zoomMax, preferences.zoomMin);
    // Version 1 kept the color with the paint tool, then a single color stood for the first slot.
    if (const auto paint = root.find("paint"); paint != root.end() && paint->is_object()) {
        Read(*paint, "color", preferences.colors[0]);
    }
    Read(root, "color", preferences.colors[0]);
    Read(root, "colors", preferences.colors, kColorSlotsMin, kColorSlotsMax);
    Read(root, "colorSlot", preferences.colorSlot);
    preferences.colorSlot = std::min(preferences.colorSlot, static_cast<std::uint32_t>(preferences.colors.size() - 1));
    Read(root, "recentColors", preferences.recentColors, 0, kRecentColorsMax);
    if (const auto ui = root.find("ui"); ui != root.end() && ui->is_object()) {
        Read(*ui, "viewShown", preferences.ui.viewShown);
        Read(*ui, "toolShown", preferences.ui.toolShown);
        Read(*ui, "toolbarShown", preferences.ui.toolbarShown);
        Read(*ui, "toolsShown", preferences.ui.toolsShown);
        Read(*ui, "layersShown", preferences.ui.layersShown);
        Read(*ui, "tagsShown", preferences.ui.tagsShown);
        Read(*ui, "colorShown", preferences.ui.colorShown);
        Read(*ui, "helpShown", preferences.ui.helpShown);
        Read(*ui, "menuBarShown", preferences.ui.menuBarShown);
        Read(*ui, "tagZoneShown", preferences.ui.tagZoneShown);
        if (const auto settings = ui->find("imguiSettings"); settings != ui->end() && settings->is_string()) {
            preferences.ui.imguiSettings = settings->get_ref<const std::string&>();
        }
    }
    if (const auto window = root.find("window"); window != root.end() && window->is_object()) {
        WindowCreateInfo& info = preferences.window;
        // Both or neither: one alone is no place.
        int x = kWindowPosDefault;
        int y = kWindowPosDefault;
        Read(*window, "x", x, kWindowPosDefault + 1, std::numeric_limits<int>::max());
        Read(*window, "y", y, kWindowPosDefault + 1, std::numeric_limits<int>::max());
        if (x != kWindowPosDefault && y != kWindowPosDefault) {
            info.x = x;
            info.y = y;
        }
        Read(*window, "width", info.width, kWindowSizeMin, kWindowSizeMax);
        Read(*window, "height", info.height, kWindowSizeMin, kWindowSizeMax);
        if (const int mode = ReadName(*window, "displayMode", kDisplayModeNames); mode >= 0) {
            info.displayMode = kDisplayModes[static_cast<std::size_t>(mode)];
        }
    }
    Read(root, "recentFiles", preferences.recentFiles, kRecentFilesMax);
    if (const auto open = root.find("openFiles"); open != root.end() && open->is_array()) {
        for (const Json& entry : *open) {
            if (preferences.openFiles.size() == kWindowMax) {
                break;
            }
            if (!entry.is_object()) {
                continue;
            }
            const auto filePath = entry.find("path");
            if (filePath == entry.end() || !filePath->is_string()) {
                continue;
            }
            OpenFile openFile{.path = PathFromUtf8(filePath->get_ref<const std::string&>()), .tagsChecked = 0};
            if (const auto checked = entry.find("tagsChecked");
                checked != entry.end() && checked->is_number_unsigned()) {
                openFile.tagsChecked = checked->get<std::uint64_t>();
            }
            preferences.openFiles.push_back(std::move(openFile));
        }
    }
    Read(root, "reopenFiles", preferences.reopenFiles);
    return preferences;
}

Result<void> PreferencesSave(const Preferences& preferences) noexcept {
    ZoneScopedN("PreferencesSave");
    const auto path = SettingsPath("preferences.json");
    if (!path) {
        return std::unexpected(path.error());
    }

    const WindowCreateInfo& window = preferences.window;
    // Only what is known: the defaults stay out of the file.
    Json windowJson{{"displayMode", DisplayModeName(window.displayMode)}};
    if (window.width > 0 && window.height > 0) {
        windowJson["width"] = window.width;
        windowJson["height"] = window.height;
    }
    if (window.x != kWindowPosDefault && window.y != kWindowPosDefault) {
        windowJson["x"] = window.x;
        windowJson["y"] = window.y;
    }
    Json openFiles = Json::array();
    for (const OpenFile& file : preferences.openFiles) {
        openFiles.push_back(Json{{"path", PathUtf8(file.path)}, {"tagsChecked", file.tagsChecked}});
    }
    const Json root{
        {"version", kPreferencesVersion},
        {"language", preferences.language},
        {"canvasFilter", kCanvasFilterNames[std::to_underlying(preferences.canvasFilter)]},
        {"zoomMin", preferences.zoomMin},
        {"zoomMax", preferences.zoomMax},
        {"colors", ColorsJson(preferences.colors)},
        {"colorSlot", preferences.colorSlot},
        {"recentColors", ColorsJson(preferences.recentColors)},
        {"ui",
         {
             {"viewShown", preferences.ui.viewShown},
             {"toolShown", preferences.ui.toolShown},
             {"toolbarShown", preferences.ui.toolbarShown},
             {"toolsShown", preferences.ui.toolsShown},
             {"layersShown", preferences.ui.layersShown},
             {"tagsShown", preferences.ui.tagsShown},
             {"colorShown", preferences.ui.colorShown},
             {"helpShown", preferences.ui.helpShown},
             {"menuBarShown", preferences.ui.menuBarShown},
             {"tagZoneShown", preferences.ui.tagZoneShown},
             {"imguiSettings", preferences.ui.imguiSettings},
         }},
        {"window", windowJson},
        {"recentFiles", PathsJson(preferences.recentFiles)},
        {"openFiles", openFiles},
        {"reopenFiles", preferences.reopenFiles},
    };
    return JsonFileWrite(*path, root);
}

ToolsState ToolsLoad(const MdslBrush& builtIn) noexcept {
    ZoneScopedN("ToolsLoad");
    ToolsState tools{};
    tools.shaders.push_back(ToolShader{.brush = builtIn});
    const auto libraryPath = SettingsPath("tools.json");
    const Json library = libraryPath ? JsonFileRead(*libraryPath) : Json();
    if (library.is_object()) {
        if (const auto list = library.find("tools"); list != library.end() && list->is_array()) {
            for (const Json& entry : *list) {
                const ToolState tool = entry.is_object() ? ReadTool(entry, tools) : ToolState{};
                if (tool.id == 0 || ToolFind(tools, tool.id)) {
                    MIDORI_LOG_WARN("tools.json: {} has no id of its own, skipped", entry.dump());
                    continue;
                }
                tools.library.push_back(tool);
            }
        }
        Read(library, "current", tools.current);
        if (const int source = ReadName(library, "eyedropperSource", kEyedropperSourceNames); source >= 0) {
            tools.eyedropperSource = static_cast<EyedropperSource>(source);
        }
    }
    if (tools.library.empty()) {
        const auto preferencesPath = SettingsPath("preferences.json");
        ToolsDefault(tools, preferencesPath ? JsonFileRead(*preferencesPath) : Json());
        return tools;
    }
    const auto workspacePath = SettingsPath("workspace.json");
    const Json workspace = workspacePath ? JsonFileRead(*workspacePath) : Json();
    if (workspace.is_object()) {
        if (const auto slots = workspace.find("toolbar"); slots != workspace.end() && slots->is_array()) {
            for (const Json& entry : *slots) {
                ToolbarSlot slot{};
                Read(entry, "id", slot.id);
                Read(entry, "last", slot.last);
                if (const auto ids = entry.find("tools"); ids != entry.end() && ids->is_array()) {
                    for (const Json& id : *ids) {
                        if (id.is_number_unsigned()
                            && id.get<std::uint64_t>() <= std::numeric_limits<std::uint32_t>::max()
                            && !std::ranges::contains(slot.tools, id.get<std::uint32_t>())) {
                            slot.tools.push_back(id.get<std::uint32_t>());
                        }
                    }
                }
                if (slot.id == 0 || ToolbarSlotFind(tools, slot.id)) {
                    MIDORI_LOG_WARN("workspace.json: {} has no id of its own, skipped", entry.dump());
                    continue;
                }
                tools.slots.push_back(std::move(slot));
            }
        }
        Read(workspace, "slot", tools.slot);
    } else {
        for (const ToolState& tool : tools.library) {
            ToolbarSlotAdd(tools, tool.id);
        }
    }
    ToolsFix(tools);
    return tools;
}

Result<void> ToolsSave(const ToolsState& tools) noexcept {
    ZoneScopedN("ToolsSave");
    const auto libraryPath = SettingsPath("tools.json");
    const auto workspacePath = SettingsPath("workspace.json");
    if (!libraryPath || !workspacePath) {
        return std::unexpected(libraryPath ? workspacePath.error() : libraryPath.error());
    }
    Json library = Json::array();
    for (const ToolState& tool : tools.library) {
        library.push_back(ToolJson(tool, tools));
    }
    Json slots = Json::array();
    for (const ToolbarSlot& slot : tools.slots) {
        slots.push_back(Json{{"id", slot.id}, {"tools", slot.tools}, {"last", slot.last}});
    }
    const auto saved = JsonFileWrite(
        *libraryPath, Json{
                          {"version", kToolsVersion},
                          {"current", tools.current},
                          {"eyedropperSource", kEyedropperSourceNames[std::to_underlying(tools.eyedropperSource)]},
                          {"tools", library},
                      });
    const auto arranged =
        JsonFileWrite(*workspacePath, Json{{"version", kToolsVersion}, {"slot", tools.slot}, {"toolbar", slots}});
    return saved ? arranged : saved;
}

} // namespace Midori
