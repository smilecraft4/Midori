#pragma once

#include "midori/action.hpp"
#include "midori/base.hpp"
#include "midori/color.hpp"
#include "midori/input.hpp"
#include "midori/platform.hpp"
#include "midori/text.hpp"
#include "midori/viewport.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct ImGuiContext;

namespace Midori {

struct ToolsState;

struct SearchEntry {
    std::array<char, 128> label; // dynamic entries only: the tool, layer, file, language or filter name
    std::uint32_t arg;
    float score;
    ActionId action;
    Text blocked; // Text::Count_ when it would run
};

// The panels a new window shows, kept between runs.
struct UiPreferences {
    std::string imguiSettings; // Dear ImGui's ini text: where its windows sit and how large they are
    bool viewShown{true};
    bool toolShown{true};
    bool toolbarShown{true};
    bool toolsShown{true};
    bool layersShown{true};
    bool helpShown{true};
    bool menuBarShown{true};
};

// One Dear ImGui context per window: its own input state, style, font atlas and draw data. Owned by
// the platform thread. Every Ui call makes the context it works on current; none relies on the one
// left current by a previous call.
struct UiState {
    std::array<ImGuiContext*, kWindowMax> contexts;  // null marks the slot free
    std::array<std::int64_t, kWindowMax> frameTimes; // steady_clock ticks of the last frame, 0 before the first
    // steady_clock ticks: the window keeps presenting until then, for ImGui to finish what its last input started.
    std::array<std::int64_t, kWindowMax> settleUntil;
    std::array<bool, kWindowMax> tileAtlasShown; // the tile atlas debug window is open
    std::array<int, kWindowMax> tileAtlasIndex;  // the atlas it shows
    std::array<bool, kWindowMax> statisticsShown;
    std::array<bool, kWindowMax> imguiMetricsShown;
    std::array<bool, kWindowMax> tileCellsShown; // the canvas view outlines its tiles with their atlas cell
    std::array<bool, kWindowMax> hidden;         // no menu, panel or debug overlay, only the brush adjust
    // The panels each window shows, from `preferences` when it opens. The help window's tips follow what the user does.
    std::array<bool, kWindowMax> helpShown;
    std::array<bool, kWindowMax> menuBarShown; // hidden, panel.menuBar from the search or a key brings it back
    std::array<bool, kWindowMax> viewShown;
    std::array<bool, kWindowMax> toolShown;
    std::array<bool, kWindowMax> toolbarShown;
    std::array<bool, kWindowMax> toolsShown;
    std::array<bool, kWindowMax> layersShown;
    std::array<bool, kWindowMax> settingsShown; // closed at first, opened from Edit > Settings
    UiPreferences preferences;                  // what a window opens with; the last window drawn sets it
    // While the opacity slider is held, where each selected layer started, top first.
    std::array<std::vector<float>, kWindowMax> layerOpacitiesBefore;
    std::array<std::vector<std::uint16_t>, kWindowMax> layerFoldersClosed; // folder layer ids
    std::array<int, kWindowMax> layerRenaming; // the layer whose name is being edited, -1 for none
    std::array<std::array<char, 64>, kWindowMax> layerRename;
    std::array<std::uint32_t, kWindowMax> toolRenaming;       // the tool whose name is being edited, 0 for none
    std::array<std::array<char, 64>, kWindowMax> slotKeys;    // a toolbar slot's keys being edited
    std::array<bool, kWindowMax> slotKeysInvalid;             // KeymapBind refused them
    std::array<bool, kWindowMax> searchShown;
    std::array<std::array<char, 128>, kWindowMax> searchQuery;
    std::array<std::vector<SearchEntry>, kWindowMax> searchEntries; // matching ones, best first
    std::array<int, kWindowMax> searchSelected;
    std::array<bool, kWindowMax> searchFocus;                       // until its query field takes the keyboard
    // Read once, used by every context's atlas without a copy; empty when not installed.
    std::vector<std::byte> fontLatin;
    std::vector<std::byte> fontJapanese; // merged into the Latin one
};

extern UiState gUiState;

// Before the first UiWindowInit. A font not found falls back to Dear ImGui's, without kana and kanji.
void UiFontsLoad() noexcept;

// Called by the platform around a window's lifetime: init before GraphicsWindowInit, terminate after
// GraphicsWindowTerminate, so the renderer backend never outlives its context.
[[nodiscard]] Result<ImGuiContext*> UiWindowInit(Window window) noexcept;
void UiWindowTerminate(Window window) noexcept;

// Queues the event into its window's context. Returns true when the UI wants it (the pointer is over
// a UI window, or a text field has focus) and the canvas should not see it. The answer comes from the
// window's last frame, so it lags the event by up to one frame.
[[nodiscard]] bool UiEvent(const Event& event) noexcept;

// The window's UI needs another frame: input came in shortly before, or a text field blinks its cursor.
[[nodiscard]] bool UiWantsFrame(Window window) noexcept;

// The cursor the UI asks for while it has the pointer, as of the window's last frame; nothing when the canvas has it.
[[nodiscard]] std::optional<WindowCursor> UiCursor(Window window) noexcept;

// Builds the window's next frame, up to ImGui::Render. The renderer then draws its draw data. The view
// panel edits `viewport` and its canvas's `backgroundColor`, and is hidden while the window shows no canvas view
// (both null). The tool panel edits `tools`. What the menus and the search pick goes to gAppState.actionRequests.
void UiWindowFrame(Window window, Viewport* viewport, Color::Srgb* backgroundColor, ToolsState& tools) noexcept;

} // namespace Midori
