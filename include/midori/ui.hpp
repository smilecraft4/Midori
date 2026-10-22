#pragma once

#include "midori/base.hpp"
#include "midori/input.hpp"
#include "midori/platform.hpp"
#include "midori/viewport.hpp"
#include <array>
#include <cstdint>

struct ImGuiContext;

namespace Midori {

struct ToolsState;

// One Dear ImGui context per window: its own input state, style, font atlas and draw data. Owned by
// the platform thread. Every Ui call makes the context it works on current; none relies on the one
// left current by a previous call.
struct UiState {
    std::array<ImGuiContext*, kWindowMax> contexts;  // null marks the slot free
    std::array<std::int64_t, kWindowMax> frameTimes; // steady_clock ticks of the last frame, 0 before the first
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
};

// Builds the window's next frame, up to ImGui::Render. The renderer then draws its draw data. The view
// panel edits `viewport` and `backgroundOpacity`, and is hidden while the window shows no canvas view (null).
// The tool panel edits `tools`.
[[nodiscard]] UiCommand UiWindowFrame(Window window, Viewport* viewport, float& backgroundOpacity,
                                      ToolsState& tools) noexcept;

} // namespace Midori
