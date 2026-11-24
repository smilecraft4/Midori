#pragma once

#include "midori/base.hpp"
#include "midori/canvas.hpp"
#include "midori/platform.hpp"
#include "midori/tile.hpp"
#include "midori/tool.hpp"
#include "midori/ui.hpp"
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Midori {

constexpr std::size_t kRecentFilesMax = 10;

// What the user keeps between runs, as preferences.json in the app's folder under PlatformUserSettingsPath: plain JSON,
// readable and editable by hand. A value missing, of the wrong kind or out of range keeps its default.
struct Preferences {
    UiPreferences ui;
    WindowCreateInfo window;                        // where the first window opens; its title unused
    CanvasFilter canvasFilter{CanvasFilter::Lanczos2};
    float tileLodBias{0.0f};                        // within [kTileLodBiasMin, kTileLodBiasMax]
    float zoomDefault{1.0f};                        // within [kViewportZoomMin, kViewportZoomMax]
    float zoomMin{kViewportZoomMin};                // within [kViewportZoomMin, kViewportZoomMax], zoomMin <= zoomMax
    float zoomMax{kViewportZoomMax};
    std::vector<std::filesystem::path> recentFiles; // the most recent first, up to kRecentFilesMax
    std::vector<OpenFile> openFiles;                // tabs at the last exit, up to kCanvasViewMax in kWindowMax windows
    std::string language;                           // a code of kLanguages, empty for the system's
    std::vector<Color::Srgb> colors{Color::Srgb{.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f},
                                    Color::Srgb{.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f}}; // ToolsState::colors
    std::vector<Color::Srgb> recentColors;
    std::uint32_t colorSlot{};
    bool reopenFiles{true}; // launched without files, opens `openFiles`
};

// The defaults while there is no file yet. A file that is not JSON is set aside as preferences.json.bad, so the next
// save does not lose it, and Error::PreferencesFailed returned.
[[nodiscard]] Result<Preferences> PreferencesLoad() noexcept;
// Written aside then renamed over the file, so a failed save leaves the last one whole.
[[nodiscard]] Result<void> PreferencesSave(const Preferences& preferences) noexcept;

// tools.json and workspace.json beside preferences.json: the tool library, and how the toolbar arranges it. Each tool's
// shader is translated as it is read, then its param values and curves by their names; `builtIn` is round.mdsl as
// built in. A shader in the brushes folder next to Midori is named from it. With no library, the default tools, an
// older preferences.json's paint brush and brush presets among them. The library is never empty.
ToolsState ToolsLoad(const MdslBrush& builtIn) noexcept;
// Each written aside then renamed over its file.
Result<void> ToolsSave(const ToolsState& tools) noexcept;

} // namespace Midori
