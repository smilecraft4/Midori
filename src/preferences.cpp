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
constexpr int kPreferencesVersion = 1;
// Names rather than numbers, so the file reads and edits by hand. Indexed by BlendMode and Tool.
constexpr std::array<std::string_view, std::to_underlying(BlendMode::Count_)> kBlendModeNames = {
    "normal", "multiply", "add", "screen", "normalOkLab", "erase",
};
constexpr std::array<std::string_view, 3> kToolNames = {"paint", "select", "lasso"};
constexpr std::array<std::string_view, std::to_underlying(BrushInput::Count_)> kBrushInputNames = {
    "none", "pressure", "tiltX", "tiltY", "speed", "random",
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

[[nodiscard]] Result<std::filesystem::path> PreferencesPath() noexcept {
    const auto settings = PlatformUserSettingsPath();
    if (!settings) {
        return std::unexpected(settings.error());
    }
    return *settings / MIDORI_NAME / "preferences.json";
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

void Read(const Json& json, const char* key, bool& value) noexcept {
    if (const auto found = json.find(key); found != json.end() && found->is_boolean()) {
        value = found->get<bool>();
    }
}

// [r, g, b, a], each from 0 to 1.
void Read(const Json& json, const char* key, Color::Srgb& color) noexcept {
    const auto found = json.find(key);
    if (found == json.end() || !found->is_array() || found->size() != 4
        || !std::ranges::all_of(*found, [](const Json& channel) { return channel.is_number(); })) {
        return;
    }
    color = Color::Srgb{
        .r = std::clamp((*found)[0].get<float>(), 0.0f, 1.0f),
        .g = std::clamp((*found)[1].get<float>(), 0.0f, 1.0f),
        .b = std::clamp((*found)[2].get<float>(), 0.0f, 1.0f),
        .a = std::clamp((*found)[3].get<float>(), 0.0f, 1.0f),
    };
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
        if (const int input = ReadName(*entry, "input", kBrushInputNames); input >= 0) {
            param.input = static_cast<BrushInput>(input);
        }
        Read(*entry, "curve", param.curve);
        // Before curves took their place, the curve's 0 to 1 scaled the value from min to max.
        float min = 0.0f;
        float max = 1.0f;
        Read(*entry, "min", min, 0.0f, 1.0f);
        Read(*entry, "max", max, min, 1.0f);
        for (std::uint32_t p = 0; p < param.curve.count; ++p) {
            param.curve.points[p].y = std::lerp(min, max, param.curve.points[p].y);
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
        paramsJson[params[i].name] = Json{
            {"value", std::vector<float>(param.value.begin(), param.value.begin() + MdslComponents(params[i].type))},
            {"input", kBrushInputNames[std::to_underlying(param.input)]},
            {"curve", CurveJson(param.curve)},
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

} // namespace

Result<Preferences> PreferencesLoad(const MdslBrush& brush) noexcept {
    ZoneScopedN("PreferencesLoad");
    Preferences preferences{};
    BrushParamsDefault(preferences.tools.paint.brush, brush);
    const auto path = PreferencesPath();
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
    if (const int tool = ReadName(root, "tool", kToolNames); tool >= 0) {
        preferences.tools.tool = static_cast<Tool>(tool);
    }
    Read(root, "zoomMin", preferences.zoomMin, kViewportZoomMin, kViewportZoomMax);
    Read(root, "zoomMax", preferences.zoomMax, kViewportZoomMin, kViewportZoomMax);
    preferences.zoomMax = std::max(preferences.zoomMax, preferences.zoomMin);
    if (const auto paint = root.find("paint"); paint != root.end() && paint->is_object()) {
        ReadBrush(*paint, preferences.tools.paint.brush, brush);
        Read(*paint, "color", preferences.tools.paint.color);
        Read(*paint, "eraseColor", preferences.tools.paint.eraseColor);
        if (const int blendMode = ReadName(*paint, "blendMode", kBlendModeNames); blendMode >= 0) {
            preferences.tools.paint.blendMode = static_cast<BlendMode>(blendMode);
        }
        if (const int blendMode = ReadName(*paint, "paintBlendMode", kBlendModeNames);
            blendMode >= 0 && blendMode != std::to_underlying(BlendMode::Erase)) {
            preferences.tools.paint.paintBlendMode = static_cast<BlendMode>(blendMode);
        }
    }
    if (const auto presets = root.find("brushPresets"); presets != root.end() && presets->is_array()) {
        for (const Json& entry : *presets) {
            if (!entry.is_object()) {
                continue;
            }
            BrushPreset preset{};
            ReadBrush(entry, preset.brush, brush);
            if (const auto name = entry.find("name"); name != entry.end() && name->is_string()) {
                const std::string& text = name->get_ref<const std::string&>();
                std::copy_n(text.begin(), std::min(text.size(), preset.name.size() - 1), preset.name.begin());
            }
            preferences.tools.presets.push_back(preset);
        }
    }
    if (const auto ui = root.find("ui"); ui != root.end() && ui->is_object()) {
        Read(*ui, "viewShown", preferences.ui.viewShown);
        Read(*ui, "toolShown", preferences.ui.toolShown);
        Read(*ui, "layersShown", preferences.ui.layersShown);
        Read(*ui, "helpShown", preferences.ui.helpShown);
        Read(*ui, "menuBarShown", preferences.ui.menuBarShown);
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
    if (const auto recent = root.find("recentFiles"); recent != root.end() && recent->is_array()) {
        for (const Json& entry : *recent) {
            if (preferences.recentFiles.size() == kRecentFilesMax) {
                break;
            }
            if (entry.is_string()) {
                const std::string& utf8 = entry.get_ref<const std::string&>();
                preferences.recentFiles.emplace_back(std::u8string(utf8.begin(), utf8.end()));
            }
        }
    }
    return preferences;
}

Result<void> PreferencesSave(const Preferences& preferences, const MdslBrush& brush) noexcept {
    ZoneScopedN("PreferencesSave");
    const auto path = PreferencesPath();
    if (!path) {
        return std::unexpected(path.error());
    }
    std::error_code ec;
    std::filesystem::create_directories(path->parent_path(), ec);
    if (ec) {
        MIDORI_LOG_ERROR("creating {} failed: {}", PathUtf8(path->parent_path()), ec.message());
        return std::unexpected(Error::PreferencesFailed);
    }

    const ToolsState& tools = preferences.tools;
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
    Json brushPresets = Json::array();
    for (const BrushPreset& preset : tools.presets) {
        brushPresets.push_back(Json{{"name", preset.name.data()}, {"brush", BrushJson(preset.brush, brush)}});
    }
    Json recentFiles = Json::array();
    for (const std::filesystem::path& file : preferences.recentFiles) {
        recentFiles.push_back(PathUtf8(file));
    }
    const Json root{
        {"version", kPreferencesVersion},
        {"language", preferences.language},
        {"tool", kToolNames[std::to_underlying(tools.tool)]},
        {"canvasFilter", kCanvasFilterNames[std::to_underlying(preferences.canvasFilter)]},
        {"zoomMin", preferences.zoomMin},
        {"zoomMax", preferences.zoomMax},
        {"paint",
         {
             {"brush", BrushJson(tools.paint.brush, brush)},
             {"color", ColorJson(tools.paint.color)},
             {"eraseColor", ColorJson(tools.paint.eraseColor)},
             {"blendMode", kBlendModeNames[std::to_underlying(tools.paint.blendMode)]},
             {"paintBlendMode", kBlendModeNames[std::to_underlying(tools.paint.paintBlendMode)]},
         }},
        {"ui",
         {
             {"viewShown", preferences.ui.viewShown},
             {"toolShown", preferences.ui.toolShown},
             {"layersShown", preferences.ui.layersShown},
             {"helpShown", preferences.ui.helpShown},
             {"menuBarShown", preferences.ui.menuBarShown},
             {"imguiSettings", preferences.ui.imguiSettings},
         }},
        {"brushPresets", brushPresets},
        {"window", windowJson},
        {"recentFiles", recentFiles},
    };
    // A file name not valid UTF-16 would make invalid UTF-8; replaced rather than failing the save.
    const std::string text = root.dump(4, ' ', false, Json::error_handler_t::replace);

    std::filesystem::path temp = *path;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) {
            MIDORI_LOG_ERROR("writing {} failed", PathUtf8(temp));
            return std::unexpected(Error::PreferencesFailed);
        }
    }
    std::filesystem::rename(temp, *path, ec);
    if (ec) {
        MIDORI_LOG_ERROR("replacing {} failed: {}", PathUtf8(*path), ec.message());
        std::filesystem::remove(temp, ec);
        return std::unexpected(Error::PreferencesFailed);
    }
    return {};
}

} // namespace Midori
