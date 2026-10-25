#pragma once

#include "midori/base.hpp"
#include "midori/canvas.hpp"
#include "midori/platform.hpp"
#include "midori/tool.hpp"
#include "midori/ui.hpp"
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace Midori {

constexpr std::size_t kRecentFilesMax = 10;

// What the user keeps between runs, as preferences.json in the app's folder under PlatformUserSettingsPath: plain JSON,
// readable and editable by hand. A value missing, of the wrong kind or out of range keeps its default.
struct Preferences {
    ToolsState tools;
    UiPreferences ui;
    WindowCreateInfo window;                        // where the first window opens; its title unused
    CanvasFilter canvasFilter{CanvasFilter::Lanczos2};
    float zoomMin{kViewportZoomMin};                // within [kViewportZoomMin, kViewportZoomMax], zoomMin <= zoomMax
    float zoomMax{kViewportZoomMax};
    std::vector<std::filesystem::path> recentFiles; // the most recent first, up to kRecentFilesMax
    std::string language;                           // a code of kLanguages, empty for the system's
};

// The defaults while there is no file yet. A file that is not JSON is set aside as preferences.json.bad, so the next
// save does not lose it, and Error::PreferencesFailed returned.
[[nodiscard]] Result<Preferences> PreferencesLoad() noexcept;
// Written aside then renamed over the file, so a failed save leaves the last one whole.
[[nodiscard]] Result<void> PreferencesSave(const Preferences& preferences) noexcept;

} // namespace Midori
