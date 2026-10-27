#pragma once

#include "midori/base.hpp"
#include "midori/canvas.hpp"
#include "midori/platform.hpp"
#include "midori/tile.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

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

// The adapter's memory, in bytes: local is the GPU's own (all of it on an integrated GPU), non-local the system memory
// it reaches. The budget is what the system lets this process use. Zero when unknown.
struct GraphicsMemory {
    std::uint64_t localUsage;
    std::uint64_t localBudget;
    std::uint64_t nonLocalUsage;
    std::uint64_t nonLocalBudget;
};

[[nodiscard]] GraphicsMemory GraphicsMemoryGet() noexcept;

enum struct GraphicsBrushStatus : std::uint8_t {
    None,  // nothing compiling, nothing to report
    Compiling,
    Ready, // compiled, painting once GraphicsBrushUse
    Failed,
};

// The stroke pipeline for a brush, from its MdslBrush::hlsl, compiled away from the main thread. One compiles at a
// time: asked for while one runs, it follows and the one before is dropped.
void GraphicsBrushCompile(std::string hlsl) noexcept;
// Ready or Failed once per compile that is not dropped; Failed sets `log` to the compiler's errors.
GraphicsBrushStatus GraphicsBrushPoll(std::string& log) noexcept;
// The last Ready pipeline paints what the next submits record. Never during a stroke: its dabs hold the old brush's
// params.
void GraphicsBrushUse() noexcept;
// A brush's pipeline is in use. Until one is, the stroke pipeline only paints the selection.
bool GraphicsBrushLoaded() noexcept;
// The image brush texture `slot` reads, below kMdslTexturesMax, its alpha straight; null for none. Outside any submit,
// and waits for the GPU.
[[nodiscard]] Result<void> GraphicsBrushTextureSet(std::uint32_t slot, const PlatformImage* image) noexcept;

// GraphicsInit creates atlas 0; the others are created in order, outside any submit. Its pages start undefined.
[[nodiscard]] Result<void> GraphicsTileAtlasCreate(TileAtlas atlas) noexcept;

constexpr TileAtlasPage kTileAtlasDebugColumns = 8;
constexpr TileAtlasPage kTileAtlasDebugRows = (kTileAtlasPages + kTileAtlasDebugColumns - 1) / kTileAtlasDebugColumns;
// A UI texture (ImTextureID) of one created atlas's pages at mip 0, kTileAtlasDebugColumns pages per row,
// refreshed in each frame whose UI called this. Created on the first call; 0 when that failed.
[[nodiscard]] std::uint64_t GraphicsTileAtlasDebugTexture(TileAtlas atlas) noexcept;

// A transfer slot's mapped memory, kTileSlotBytes, valid from GraphicsInit to GraphicsTerminate. Each submit records
// the copies of the slots in TileSlotState::Record (gAppState.transfers) and moves them to Gpu with the fence it
// signals; a job may use the memory from another thread while its slot is in TileSlotState::Job.
[[nodiscard]] std::byte* GraphicsTileUploadMemory(TileTransferSlot slot) noexcept;
[[nodiscard]] std::byte* GraphicsTileDownloadMemory(TileTransferSlot slot) noexcept;
// The GPU is done with everything recorded before a fence value up to this one.
[[nodiscard]] std::uint64_t GraphicsCompletedFence() noexcept;

// Waits for the GPU. A TileBytes stride per cell, each tile TileLevelBytes of its level, both in the atlas's RGBA8.
// First records pending new tiles, uploads, the stroke and level builds so far.
[[nodiscard]] Result<void> GraphicsTilesDownload(std::span<const TileAtlasCell> cells,
                                                 std::span<std::byte> pixels) noexcept;

} // namespace Midori
