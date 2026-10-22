#pragma once

#include "midori/base.hpp"
#include "midori/canvas.hpp"
#include "midori/platform.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

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

constexpr TileAtlasCell kTileAtlasDebugColumns = 8;
constexpr TileAtlasCell kTileAtlasDebugRows = (kTileAtlasCellMax + kTileAtlasDebugColumns - 1) / kTileAtlasDebugColumns;
// A UI texture (ImTextureID) of every atlas cell at mip 0, kTileAtlasDebugColumns cells per row, refreshed in
// each frame whose UI called this. Created on the first call; 0 when that failed.
[[nodiscard]] std::uint64_t GraphicsTileAtlasDebugTexture() noexcept;

// Both wait for the GPU. Pixels are kTileBytes per cell, mip 0 only. A download first fills pending new tiles and
// records the stroke so far.
[[nodiscard]] Result<void> GraphicsTilesDownload(std::span<const TileAtlasCell> cells,
                                                 std::span<std::byte> pixels) noexcept;
[[nodiscard]] Result<void> GraphicsTilesUpload(std::span<const TileAtlasCell> cells,
                                               std::span<const std::byte* const> pixels) noexcept;

} // namespace Midori
