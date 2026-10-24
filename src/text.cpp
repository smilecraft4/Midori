#include "midori/text.hpp"

#include "midori/log.hpp"
#include "midori/platform.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <json.hpp>
#include <tracy/Tracy.hpp>

namespace Midori {

TextState gTextState{};

namespace {

using Json = nlohmann::json;

// The printf conversions in order, flags, widths and precisions included.
[[nodiscard]] std::string FormatSpecifiers(std::string_view text) noexcept {
    std::string specifiers;
    for (std::size_t i = text.find('%'); i != std::string_view::npos; i = text.find('%', i + 1)) {
        const std::size_t end = text.find_first_of("diouxXeEfFgGaAcspn%", i + 1);
        specifiers += text.substr(i, end == std::string_view::npos ? end : end - i + 1);
        if (end == std::string_view::npos) {
            break;
        }
        i = end;
    }
    return specifiers;
}

[[nodiscard]] Json LanguageRead(std::string_view language) noexcept {
    if (language.empty() || language == "en") {
        return Json::object();
    }
    const auto folder = PlatformExecutableFolder();
    if (!folder) {
        return Json::object();
    }
    const std::filesystem::path path = *folder / "lang" / (std::string(language) + ".json");
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        MIDORI_LOG_WARN("no {} translation at {}, English used", language, PathUtf8(path));
        return Json::object();
    }
    Json root = Json::parse(file, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        MIDORI_LOG_WARN("{} is not a JSON object, English used", PathUtf8(path));
        return Json::object();
    }
    return root;
}

} // namespace

void TextLoad(std::string_view language) noexcept {
    ZoneScopedN("TextLoad");
    const Json root = LanguageRead(language);
    TextState& state = gTextState;
    state.language = language;
    state.arena.clear();
    std::size_t missing = 0;
    const auto translated = [&](const char* key, std::string_view english) -> std::string_view {
        const auto found = root.find(key);
        if (found == root.end() || !found->is_string()) {
            ++missing;
            return english;
        }
        const std::string& text = found->get_ref<const std::string&>();
        if (FormatSpecifiers(text) != FormatSpecifiers(english)) {
            MIDORI_LOG_WARN("{} translation of {} changes its format specifiers, English used", language, key);
            return english;
        }
        return text;
    };
    const auto append = [&](std::string_view text) {
        const auto offset = static_cast<std::uint32_t>(state.arena.size());
        state.arena += text;
        state.arena += '\0';
        return offset;
    };

    for (std::size_t i = 0; i < kTexts.size(); ++i) {
        const std::string_view text = translated(kTexts[i].key, kTexts[i].english);
        state.texts[i] = append(text);
        state.labels[i] = append(std::string(text) + "###" + kTexts[i].key);
    }
    for (std::size_t i = 0; i < kActions.size(); ++i) {
        state.actions[i] = append(translated(kActions[i].name, kActions[i].description));
    }
    if (!root.empty() && missing > 0) {
        MIDORI_LOG_WARN("{} translation misses {} texts, English used for them", language, missing);
    }
}

} // namespace Midori
