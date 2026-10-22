#pragma once

#include "midori/canvas.hpp"
#include <cstdint>
#include <vector>

namespace Midori {

struct StrokeState;

// Slots of the history store, a GPU buffer of 32^2 RGBA8 blocks without mips that every canvas shares.
constexpr std::uint32_t kHistorySlotsMax = 16384; // 64 MiB
constexpr std::uint32_t kHistoryBlockBytes = kTileAtlasBlockSize * kTileAtlasBlockSize * 4;

// One block copy between a level-0 cell and a slot.
struct HistoryOp {
    std::uint32_t slot;
    TileAtlasCell cell;
    std::uint8_t block; // Morton index in the cell
    bool swap;          // false: the cell's block goes into the slot; true: they trade places
};

// Owned by the main thread, in AppState. The next submit records `ops` before anything else, so a cell freed after
// queuing its copy can be taken again: whatever fills it is recorded later. Pushes and steps wait for `ops` to be
// recorded, so no slot is in two ops of one dispatch.
struct HistoryPoolState {
    std::vector<std::uint32_t> freeSlots;
    std::vector<HistoryOp> ops; // at most kHistorySlotsMax
    std::uint64_t sequence{};   // records pushed so far
};

// Archives the blocks the stroke touched in the tiles it backed up, and notes the tiles it created, as one record of
// its canvas after dropping what could be redone. The oldest records of any canvas go while the store has too few
// free slots. The stroke must have no op waiting.
void HistoryPushStroke(StrokeState& stroke) noexcept;
// Once per update, after the frame's visible tiles are marked: applies the canvas's next pending step once no stroke
// is on it, no op is waiting and the step's tiles are loaded, keeping them wanted meanwhile. Steps waiting too long
// are dropped.
void HistoryUpdate(Canvas canvas) noexcept;

} // namespace Midori
