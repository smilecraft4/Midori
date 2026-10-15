#pragma once

#include "midori/base.hpp"
#include "midori/canvas.hpp"
#include "midori/input.hpp"
#include "midori/platform.hpp"
#include <array>
#include <string_view>

namespace Midori {

struct AppInitInfo {};

struct AppState {
    std::array<WindowState, kWindowMax> windows;
    std::array<CanvasState, kCanvasMax> canvases;
    std::array<CanvasState, kCanvasViewMax> canvasViews;
};

[[nodiscard]] Result<void> AppInit(AppInitInfo initInfo) noexcept;
[[nodiscard]] Result<void> AppEvent(Event event) noexcept;
void AppTerminate() noexcept;

[[nodiscard]] Result<void> AppCanvasNew(Window window) noexcept;
[[nodiscard]] Result<void> AppCanvasOpen(Window window, std::string_view filename) noexcept;
[[nodiscard]] Result<void> AppCanvasSave(Canvas canvas) noexcept;
[[nodiscard]] Result<void> AppCanvasSaveAs(Canvas canvas, std::string_view filename) noexcept;

[[nodiscard]] Result<void> AppPaintMode() noexcept;
[[nodiscard]] Result<void> AppEraseMode() noexcept;
[[nodiscard]] Result<void> AppSelectMode() noexcept;
[[nodiscard]] Result<void> AppTransformMode() noexcept;

} // namespace Midori
