#include "midori/action.hpp"

#include "midori/log.hpp"
#include "midori/platform.hpp"
#include "midori/version.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <json.hpp>
#include <tracy/Tracy.hpp>

namespace Midori {

namespace {

using Json = nlohmann::json;

struct ModName {
    std::string_view name;
    KeyMod mod;
    Key key; // whose layout name shows the modifier
};

// In the order a chord shows them.
constexpr std::array kModNames = {
    ModName{.name = "Ctrl", .mod = KeyMod::Ctrl, .key = Key::LeftCtrl},
    ModName{.name = "Shift", .mod = KeyMod::Shift, .key = Key::LeftShift},
    ModName{.name = "Alt", .mod = KeyMod::Alt, .key = Key::LeftAlt},
    ModName{.name = "Super", .mod = KeyMod::Super, .key = Key::LeftSuper},
};

[[nodiscard]] std::string_view Trim(std::string_view text) noexcept {
    const std::size_t first = text.find_first_not_of(' ');
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(' ') - first + 1);
}

[[nodiscard]] char Lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b, [](char x, char y) { return Lower(x) == Lower(y); });
}

[[nodiscard]] std::string ChordKeys(KeyChord chord) noexcept {
    std::string keys;
    for (const ModName& mod : kModNames) {
        if (HasAnyFlag(chord.mods, mod.mod)) {
            keys += PlatformKeyName(mod.key);
            keys += '+';
        }
    }
    keys += PlatformKeyName(chord.key);
    return keys;
}

// Either side of a modifier as both, as bindings write them.
[[nodiscard]] KeyMod ModsGeneric(KeyMod mods) noexcept {
    KeyMod generic = KeyMod::None;
    for (const ModName& mod : kModNames) {
        if (HasAnyFlag(mods, mod.mod)) {
            generic |= mod.mod;
        }
    }
    return generic;
}

// 2 for the same modifiers, 1 when they differ only by some of `carried`, 0 for no match.
[[nodiscard]] int ChordMatch(KeyChord bound, KeyChord pressed, KeyMod carried) noexcept {
    if (bound.key != pressed.key) {
        return 0;
    }
    if (bound.mods == pressed.mods) {
        return 2;
    }
    return (bound.mods & ~carried) == (pressed.mods & ~carried) ? 1 : 0;
}

// One fires whenever the other would, or before it could.
[[nodiscard]] bool BindingsConflict(const KeyBinding& a, const KeyBinding& b) noexcept {
    if (a.context != b.context) {
        return false;
    }
    for (std::uint8_t i = 0; i < std::min(a.length, b.length); ++i) {
        if (a.chords[i].key != b.chords[i].key || a.chords[i].mods != b.chords[i].mods) {
            return false;
        }
    }
    return true;
}

// keybinds.json's entries. When it is missing an empty array is written there, so the user has a file to edit.
[[nodiscard]] Json BindingsRead() noexcept {
    const auto settings = PlatformUserSettingsPath();
    if (!settings) {
        return Json::array();
    }
    const std::filesystem::path path = *settings / MIDORI_NAME / "keybinds.json";
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "[]\n";
        if (!out) {
            MIDORI_LOG_WARN("writing {} failed", PathUtf8(path));
        }
        return Json::array();
    }
    Json root = Json::parse(file, nullptr, false);
    if (root.is_discarded() || !root.is_array()) {
        MIDORI_LOG_WARN("{} is not a JSON array, only the default bindings are used", PathUtf8(path));
        return Json::array();
    }
    return root;
}

// The sum of ChordMatch over the first `count` pending chords, -1 when one does not match.
[[nodiscard]] int PendingMatch(const KeymapState& keymap, const KeyBinding& binding, std::uint8_t count) noexcept {
    int score = 0;
    for (std::uint8_t c = 0; c < count; ++c) {
        const KeyMod carried = c == 0 ? KeyMod::None : keymap.pending[c].mods & keymap.pending[c - 1].mods;
        const int match = ChordMatch(binding.chords[c], keymap.pending[c], carried);
        if (match == 0) {
            return -1;
        }
        score += match;
    }
    return score;
}

[[nodiscard]] bool IsLower(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

// fzy's bonus for matching `c` right after `previous`.
[[nodiscard]] float FuzzyBonus(char previous, char c) noexcept {
    const bool upper = c >= 'A' && c <= 'Z';
    if (!upper && !IsLower(c)) {
        return 0.0f;
    }
    switch (previous) {
        case '/':
            return 0.9f;
        case '-':
        case '_':
        case ' ':
            return 0.8f;
        case '.':
            return 0.6f;
        default:
            return upper && IsLower(previous) ? 0.7f : 0.0f;
    }
}

} // namespace

Result<KeyBinding> KeyBindingParse(std::string_view keys) noexcept {
    KeyBinding binding{.length = 0};
    for (;;) {
        const std::size_t comma = keys.find(',');
        std::string_view chord = Trim(keys.substr(0, comma));
        if (chord.empty() || binding.length == kKeySequenceMax) {
            return std::unexpected(Error::KeyBindingInvalid);
        }
        KeyChord& out = binding.chords[binding.length++];
        out = KeyChord{.key = Key::Unknown, .mods = KeyMod::None};
        for (std::size_t plus = chord.find('+'); plus != std::string_view::npos; plus = chord.find('+')) {
            const std::string_view name = Trim(chord.substr(0, plus));
            const auto mod =
                std::ranges::find_if(kModNames, [&](const ModName& m) { return EqualsIgnoreCase(m.name, name); });
            if (mod == kModNames.end()) {
                return std::unexpected(Error::KeyBindingInvalid);
            }
            out.mods |= mod->mod;
            chord.remove_prefix(plus + 1);
        }
        const std::string_view name = Trim(chord);
        const auto key = std::ranges::find_if(kKeyNames, [&](std::string_view k) { return EqualsIgnoreCase(k, name); });
        if (key == kKeyNames.end() || key == kKeyNames.begin()) {
            return std::unexpected(Error::KeyBindingInvalid);
        }
        out.key = static_cast<Key>(key - kKeyNames.begin());
        if (comma == std::string_view::npos) {
            return binding;
        }
        keys.remove_prefix(comma + 1);
    }
}

void KeymapLoad(KeymapState& keymap) noexcept {
    ZoneScopedN("KeymapLoad");
    keymap = KeymapState{};
    for (const KeyBindingText& text : kDefaultBindings) {
        auto binding = KeyBindingParse(text.keys);
        if (!binding) {
            MIDORI_LOG_ERROR("default binding \"{}\": {}", text.keys, ErrorMessage(binding.error()));
            continue;
        }
        binding->arg = text.arg;
        binding->action = text.action;
        binding->context = text.context;
        keymap.bindings.push_back(*binding);
    }

    // Each entry takes the place of the bindings it conflicts with, the defaults' or an earlier entry's; "none" only
    // unbinds them.
    const Json root = BindingsRead();
    for (const Json& entry : root) {
        const auto keys = entry.is_object() ? entry.find("keys") : entry.end();
        const auto action = entry.is_object() ? entry.find("action") : entry.end();
        if (keys == entry.end() || action == entry.end() || !keys->is_string() || !action->is_string()) {
            MIDORI_LOG_WARN("keybinds.json: {} needs a \"keys\" and an \"action\" string, skipped", entry.dump());
            continue;
        }
        const bool none = action->get_ref<const std::string&>() == "none";
        const auto info = std::ranges::find(kActions, action->get_ref<const std::string&>(), &ActionInfo::name);
        auto binding = KeyBindingParse(keys->get_ref<const std::string&>());
        if ((info == kActions.end() && !none) || !binding) {
            MIDORI_LOG_WARN("keybinds.json: {}: {}, skipped", entry.dump(),
                            info == kActions.end() ? "no such action" : ErrorMessage(binding.error()));
            continue;
        }
        binding->action = none ? ActionId::Count_ : static_cast<ActionId>(info - kActions.begin());
        if (const auto arg = entry.find("arg"); arg != entry.end() && arg->is_number_unsigned()) {
            binding->arg = arg->get<std::uint32_t>();
        }
        if (const auto context = entry.find("context"); context != entry.end()) {
            const auto name = context->is_string()
                                ? std::ranges::find(kContextNames, context->get_ref<const std::string&>())
                                : kContextNames.end();
            if (name == kContextNames.end()) {
                MIDORI_LOG_WARN("keybinds.json: {}: no such context, skipped", entry.dump());
                continue;
            }
            binding->context = static_cast<ActionContext>(name - kContextNames.begin());
        }
        std::erase_if(keymap.bindings, [&](const KeyBinding& b) { return BindingsConflict(b, *binding); });
        if (!none) {
            keymap.bindings.push_back(*binding);
        }
    }

    // A Navigate binding only works while view.navigate is held, so it shows with its keys first: "Space+R".
    const auto navigate = std::ranges::find(keymap.bindings, ActionId::ViewNavigate, &KeyBinding::action);
    const std::string navigateKeys = navigate == keymap.bindings.end() ? std::string() : ChordKeys(navigate->chords[0]);
    for (const KeyBinding& binding : keymap.bindings) {
        std::string keys;
        if (binding.context == ActionContext::Navigate && !navigateKeys.empty()) {
            keys = navigateKeys + '+';
        }
        for (std::uint8_t i = 0; i < binding.length; ++i) {
            if (i > 0) {
                keys += ", ";
            }
            keys += ChordKeys(binding.chords[i]);
        }
        keymap.keys.push_back(std::move(keys));
    }
}

const char* KeymapKeys(const KeymapState& keymap, ActionId action) noexcept {
    const auto binding = std::ranges::find(keymap.bindings, action, &KeyBinding::action);
    if (binding == keymap.bindings.end()) {
        return "";
    }
    return keymap.keys[static_cast<std::size_t>(binding - keymap.bindings.begin())].c_str();
}

std::int32_t KeymapPress(KeymapState& keymap, KeyChord chord, ActionContext context) noexcept {
    chord.mods = ModsGeneric(chord.mods);
    // A chord that ends no sequence in progress cancels it, then may start one of its own.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const std::uint8_t length = keymap.pendingLength;
        keymap.pending[length] = chord;
        std::int32_t best = -1;
        int bestScore = 0;
        bool longer = false;
        for (std::size_t i = 0; i < keymap.bindings.size(); ++i) {
            const KeyBinding& binding = keymap.bindings[i];
            if (binding.context > context || binding.length <= length) {
                continue;
            }
            // An inner context outweighs any chord matches: 2 per exact chord at most.
            int score = PendingMatch(keymap, binding, static_cast<std::uint8_t>(length + 1));
            score = score < 0
                      ? -1
                      : score + (std::to_underlying(binding.context) * static_cast<int>((2 * kKeySequenceMax) + 1));
            if (score < 0) {
                continue;
            }
            if (binding.length > length + 1) {
                longer = true;
            } else if (score > bestScore) {
                best = static_cast<std::int32_t>(i);
                bestScore = score;
            }
        }
        if (best >= 0) {
            keymap.pendingLength = 0;
            return best;
        }
        if (longer) {
            keymap.pendingLength = static_cast<std::uint8_t>(length + 1);
            return -1;
        }
        keymap.pendingLength = 0;
        if (length == 0) {
            return -1;
        }
    }
    return -1;
}

bool KeymapContinues(const KeymapState& keymap, const KeyBinding& binding) noexcept {
    return binding.length > keymap.pendingLength && PendingMatch(keymap, binding, keymap.pendingLength) >= 0;
}

float FuzzyScore(std::string_view needle, std::string_view haystack) noexcept {
    constexpr float kGapLeading = -0.005f;
    constexpr float kGapTrailing = -0.005f;
    constexpr float kGapInner = -0.01f;
    constexpr float kConsecutive = 1.0f;
    const std::size_t n = needle.size();
    const std::size_t m = std::min(haystack.size(), kFuzzyLengthMax);
    if (n == 0) {
        return 0.0f;
    }
    if (n > m) {
        return kFuzzyNone;
    }
    std::array<char, kFuzzyLengthMax> lower{};
    std::array<float, kFuzzyLengthMax> bonus{};
    char previous = '/';
    for (std::size_t j = 0; j < m; ++j) {
        lower[j] = Lower(haystack[j]);
        bonus[j] = FuzzyBonus(previous, haystack[j]);
        previous = haystack[j];
    }
    // Row by row over the needle, in place: d[j] is the best score with needle[i] matched at j, best[j] the best
    // score of needle[0..i] within haystack[0..j].
    std::array<float, kFuzzyLengthMax> d{};
    std::array<float, kFuzzyLengthMax> best{};
    d.fill(kFuzzyNone);
    best.fill(kFuzzyNone);
    for (std::size_t i = 0; i < n; ++i) {
        const char c = Lower(needle[i]);
        const float gap = i == n - 1 ? kGapTrailing : kGapInner;
        float rowBest = kFuzzyNone;
        float previousD = kFuzzyNone;
        float previousBest = kFuzzyNone;
        for (std::size_t j = 0; j < m; ++j) {
            float score = kFuzzyNone;
            if (lower[j] == c) {
                if (i == 0) {
                    score = (static_cast<float>(j) * kGapLeading) + bonus[j];
                } else if (j > 0) {
                    score = std::max(previousBest + bonus[j], previousD + kConsecutive);
                }
            }
            previousD = d[j];
            previousBest = best[j];
            d[j] = score;
            rowBest = std::max(score, rowBest + gap);
            best[j] = rowBest;
        }
    }
    return best[m - 1] < kFuzzyNone * 0.5f ? kFuzzyNone : best[m - 1];
}

float FuzzyScoreTerms(std::string_view query, std::string_view haystack) noexcept {
    float total = 0.0f;
    for (std::size_t start = 0; start < query.size();) {
        const std::size_t end = std::min(query.find(' ', start), query.size());
        if (end > start) {
            const float score = FuzzyScore(query.substr(start, end - start), haystack);
            if (score == kFuzzyNone) {
                return kFuzzyNone;
            }
            total += score;
        }
        start = end + 1;
    }
    return total;
}

} // namespace Midori
