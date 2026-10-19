#pragma once

#include "midori/base.hpp"
#include "midori/platform.hpp"

struct ImGuiContext;

namespace Midori {

// Graphics calls belong to the thread that called GraphicsInit, the same one as platform calls.

// Defined by the backend (win32_graphics.hpp).
struct GraphicsInitInfo;

[[nodiscard]] Result<void> GraphicsInit(GraphicsInitInfo initInfo) noexcept;
// Presents every window that needs a frame and can take one without blocking.
[[nodiscard]] Result<void> GraphicsUpdate() noexcept;
// Waits for the GPU to go idle, then releases everything.
void GraphicsTerminate() noexcept;

struct GraphicsWindowInitInfo {
    ImGuiContext* ui; // the window's UI context; its renderer backend lives until GraphicsWindowTerminate
    Window window;
};

[[nodiscard]] Result<void> GraphicsWindowInit(GraphicsWindowInitInfo initInfo) noexcept;
// Presents the window now, waiting until it can take a frame.
[[nodiscard]] Result<void> GraphicsWindowPresent(Window window) noexcept;
[[nodiscard]] Result<void> GraphicsWindowTerminate(Window window) noexcept;
// The window gets a frame at its next vblank, from GraphicsUpdate.
void GraphicsWindowRequestFrame(Window window) noexcept;

} // namespace Midori
