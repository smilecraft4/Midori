#pragma once

#include "midori/base.hpp"
#include "midori/color.hpp"
#include "midori/input.hpp"
#include "midori/platform.hpp"
#include "midori/viewport.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

struct ImGuiContext;

namespace Midori {

struct ToolsState;

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
    std::array<bool, kWindowMax> helpShown;   // the help window, shown at first; its tips follow what the user does
    std::array<bool, kWindowMax> viewShown;   // the view and tool windows, shown at first
    std::array<bool, kWindowMax> toolShown;
    std::array<bool, kWindowMax> layersShown; // the layers window, shown at first
    // While the opacity slider is held, where each selected layer started, top first.
    std::array<std::vector<float>, kWindowMax> layerOpacitiesBefore;
    std::array<std::vector<std::uint16_t>, kWindowMax> layerFoldersClosed; // folder layer ids
    std::array<int, kWindowMax> layerRenaming; // the layer whose name is being edited, -1 for none
    std::array<std::array<char, 64>, kWindowMax> layerRename;
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
