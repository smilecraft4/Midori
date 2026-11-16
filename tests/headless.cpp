// The platform functions the core files the tests and benchmark use call, without a window, a GPU or the user's
// settings: nothing on disk is read or written, keys keep their keybinds.json names, and an assert aborts.

#include "midori/input.hpp"
#include "midori/log.hpp"
#include "midori/platform.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

namespace Midori {

void AssertAbort() noexcept {
    spdlog::default_logger_raw()->flush();
    std::abort();
}

std::string PlatformKeyName(Key key) noexcept {
    return std::string(key < Key::Count_ ? kKeyNames[std::to_underlying(key)] : kKeyNames[0]);
}

Result<std::filesystem::path> PlatformUserSettingsPath() noexcept {
    return std::unexpected(Error::PlatformInitFailed);
}

Result<std::filesystem::path> PlatformExecutableFolder() noexcept {
    return std::unexpected(Error::PlatformInitFailed);
}

std::string PlatformLanguage() noexcept {
    return "en";
}

} // namespace Midori
