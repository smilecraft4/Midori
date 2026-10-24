#pragma once

#include "midori/action.hpp"
#include "midori/base.hpp"
#include "midori/color.hpp"
#include "midori/input.hpp"
#include "midori/platform.hpp"
#include "midori/viewport.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct ImGuiContext;

namespace Midori {

struct ToolsState;

// The panels a new window shows, kept between runs.
struct UiPreferences {
    std::string imguiSettings; // Dear ImGui's ini text: where its windows sit and how large they are
    bool viewShown{true};
    bool toolShown{true};
    bool layersShown{true};
    bool helpShown{true};
};

// One Dear ImGui context per window: its own input state, style, font atlas and draw data. Owned by
// the platform thread. Every Ui call makes the context it works on current; none relies on the one
// left current by a previous call.
struct UiState {
    std::array<ImGuiContext*, kWindowMax> contexts;  // null marks the slot free
    std::array<std::int64_t, kWindowMax> frameTimes; // steady_clock ticks of the last frame, 0 before the first
    std::array<bool, kWindowMax> tileAtlasShown;     // the tile atlas debug window is open
    std::array<int, kWindowMax> tileAtlasIndex;      // the atlas it shows
    std::array<bool, kWindowMax> tileCellsShown;     // the canvas view outlines its tiles with their atlas cell
    std::array<bool, kWindowMax> hidden;             // no menu, panel or debug overlay, only the brush adjust
    // The panels each window shows, from `preferences` when it opens. The help window's tips follow what the user does.
    std::array<bool, kWindowMax> helpShown;
    std::array<bool, kWindowMax> viewShown;
    std::array<bool, kWindowMax> toolShown;
    std::array<bool, kWindowMax> layersShown;
    std::array<bool, kWindowMax> settingsShown; // closed at first, opened from Edit > Settings
    UiPreferences preferences;                  // what a window opens with; the last window drawn sets it
    // While the opacity slider is held, where each selected layer started, top first.
    std::array<std::vector<float>, kWindowMax> layerOpacitiesBefore;
    std::array<std::vector<std::uint16_t>, kWindowMax> layerFoldersClosed; // folder layer ids
    std::array<int, kWindowMax> layerRenaming; // the layer whose name is being edited, -1 for none
    std::array<std::array<char, 64>, kWindowMax> layerRename;
    std::array<bool, kWindowMax> searchShown;
    std::array<std::array<char, 128>, kWindowMax> searchQuery;
    std::array<std::vector<SearchEntry>, kWindowMax> searchEntries; // matching ones, best first
    std::array<int, kWindowMax> searchSelected;
};

extern UiState gUiState;

// Called by the platform around a window's lifetime: init before GraphicsWindowInit, terminate after
// GraphicsWindowTerminate, so the renderer backend never outlives its context.
[[nodiscard]] Result<ImGuiContext*> UiWindowInit(Window window) noexcept;
void UiWindowTerminate(Window window) noexcept;

// Queues the event into its window's context. Returns true when the UI wants it (the pointer is over
// a UI window, or a text field has focus) and the canvas should not see it. The answer comes from the
// window's last frame, so it lags the event by up to one frame.
[[nodiscard]] bool UiEvent(const Event& event) noexcept;

// Picked from the menu bar. The frame is built inside the renderer's submit, where no dialog can open, so
// the app runs it later.
enum struct UiCommand : std::uint8_t {
    None,
    CanvasNew,
    CanvasOpen,
    CanvasSave,
    CanvasSaveAs,
    CanvasOpenRecent,
    Exit,
    Undo,
    Redo,
    ToggleFullscreen,
};

// Builds the window's next frame, up to ImGui::Render. The renderer then draws its draw data. The view
// panel edits `viewport` and its canvas's `backgroundColor`, and is hidden while the window shows no canvas view
// (both null). The tool panel edits `tools`.
// The cursor the UI asks for while it has the pointer, as of the window's last frame; nothing when the canvas has it.
[[nodiscard]] std::optional<WindowCursor> UiCursor(Window window) noexcept;

[[nodiscard]] UiCommand UiWindowFrame(Window window, Viewport* viewport, Color::Srgb* backgroundColor,
                                      ToolsState& tools) noexcept;

} // namespace Midori
