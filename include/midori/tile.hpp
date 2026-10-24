#pragma once

#include "midori/base.hpp"
#include "midori/canvas.hpp"
#include "midori/color.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace Midori {

using TileAxis = std::int32_t;
constexpr TileAxis kTileSize = 256; // canvas units, and texels at mip 0
static_assert(kViewportPosMax * 2.0 <= static_cast<double>(std::int64_t{1} << (kTileXBitSize - 1)) * kTileSize
                  && kTileXBitSize == kTileYBitSize,
              "a canvas point in reach of the view has a tile key");

// A level-k tile covers the same kTileSize canvas units at kTileSize >> k texels, box filtered from level 0. It
// exists only while it matches its level-0 tile: an edit deletes it, and it is rebuilt from level 0 later.
using TileMip = std::uint8_t;
constexpr TileMip kTileMipMax = 0b11 + 1; // 256, 128, 64, 32

[[nodiscard]] constexpr std::uint32_t TileLevelSize(TileMip level) noexcept {
    return std::uint32_t{kTileSize} >> level;
}

// Rows padded to the 256 bytes GPU copies need: the layout of a level-k tile in transfer slots, the cache and the
// canvas file. Every level fits in TileBytes.
[[nodiscard]] constexpr std::size_t TileLevelPitch(TileMip level, PixelFormat format) noexcept {
    return std::max<std::size_t>(std::size_t{TileLevelSize(level)} * PixelFormatTexelBytes(format), 256);
}

[[nodiscard]] constexpr std::size_t TileLevelBytes(TileMip level, PixelFormat format) noexcept {
    return TileLevelPitch(level, format) * TileLevelSize(level);
}

[[nodiscard]] constexpr std::size_t TileBytes(PixelFormat format) noexcept {
    return TileLevelBytes(0, format);
}

struct TileGridPos {
    TileAxis x{};
    TileAxis y{};
};

struct Tile {
    Canvas canvas{kCanvasInvalid};
    TileMip mip{};
    Layer layer{kLayerInvalid};
    TileGridPos gridPos{};
};

// Flipping the sign bit of x and y (offset binary) keeps -1 next to 0 in sorted order.
constexpr TileKey TileKeyMake(Layer layer, TileMip mip, TileGridPos gridPos) noexcept {
    return ((static_cast<TileKey>(static_cast<std::uint32_t>(gridPos.x) & ((TileKey{1} << kTileXBitSize) - 1))
             ^ (TileKey{1} << (kTileXBitSize - 1)))
            << kTileXBitOffset)
         | ((static_cast<TileKey>(static_cast<std::uint32_t>(gridPos.y) & ((TileKey{1} << kTileYBitSize) - 1))
             ^ (TileKey{1} << (kTileYBitSize - 1)))
            << kTileYBitOffset)
         | (static_cast<TileKey>(mip) << kTileMipBitOffset) | //
           (static_cast<TileKey>(layer) << kTileLayerBitOffset);
}

constexpr TileGridPos TileKeyGridPos(TileKey key) noexcept {
    const auto axis = [key](std::size_t offset, std::size_t size) {
        const auto stored = static_cast<std::int64_t>((key >> offset) & ((TileKey{1} << size) - 1));
        return static_cast<TileAxis>(stored - (std::int64_t{1} << (size - 1)));
    };
    return TileGridPos{.x = axis(kTileXBitOffset, kTileXBitSize), .y = axis(kTileYBitOffset, kTileYBitSize)};
}

constexpr TileKey TileGridPosKey(TileKey key) noexcept {
    return key & ~kTileLayerMask;
}

constexpr TileKey TileGridPosLayerKey(TileKey gridPosKey, Layer layer) noexcept {
    return gridPosKey | (static_cast<TileKey>(layer) << kTileLayerBitOffset);
}

constexpr Layer TileKeyLayer(TileKey key) noexcept {
    return static_cast<Layer>(key >> kTileLayerBitOffset);
}

constexpr TileMip TileKeyMip(TileKey key) noexcept {
    return static_cast<TileMip>((key >> kTileMipBitOffset) & ((TileKey{1} << kTileMipBitSize) - 1));
}

constexpr TileKey TileKeyWithMip(TileKey key, TileMip mip) noexcept {
    const TileKey mask = ((TileKey{1} << kTileMipBitSize) - 1) << kTileMipBitOffset;
    return (key & ~mask) | (static_cast<TileKey>(mip) << kTileMipBitOffset);
}

// The level a view shows at this zoom: one texel per DIP or more.
[[nodiscard]] inline TileMip TileLevelForZoom(float zoom) noexcept {
    const float level = std::floor(std::log2(1.0f / std::abs(zoom)));
    return static_cast<TileMip>(std::clamp(level, 0.0f, static_cast<float>(kTileMipMax - 1)));
}

// Its current content is in the cache or the file, so its atlas cell can be taken.
[[nodiscard]] constexpr bool TilePersisted(TileFlags flags) noexcept {
    return HasAnyFlag(flags, TileFlags::CachedDisk)
        || (HasAnyFlag(flags, TileFlags::Saved) && !HasAnyFlag(flags, TileFlags::Modified));
}

// A cell of 256 >> level for the tile `key` of `canvas` (kCanvasInvalid: held by no tile, never evicted). Best
// fit into the fullest page with room, else a new atlas while fewer than kTileAtlasMax exist, else the aligned
// run whose owners were shown the longest ago, all persisted, all evicted. A stroke's tiles are never evicted.
// kTileAtlasCellInvalid when no cell can be taken.
[[nodiscard]] TileAtlasCell TileAtlasCellTake(std::uint8_t level, Canvas canvas, TileKey key) noexcept;
void TileAtlasCellFree(TileAtlasCell cell) noexcept;
// Hands a taken cell to the tile `key` of `canvas`, or to no tile with kCanvasInvalid.
void TileAtlasCellOwn(TileAtlasCell cell, Canvas canvas, TileKey key) noexcept;

// A new tile takes an atlas cell, evicting one if needed, and starts transparent. Creating a tile
// that exists or deleting one that does not does nothing. Deleting a level-0 tile deletes its level 1-3 tiles.
[[nodiscard]] Result<void> TileNew(Tile tile) noexcept;
[[nodiscard]] Result<void> TileDelete(Tile tile) noexcept;
// The level 1-3 tiles of the level-0 tile `key`.
void TileMipsDelete(Canvas canvas, TileKey key) noexcept;
// Adds a Modified tile holding `cell`, leaving its content to the caller: TileNew queues the clear copy, mip builds
// their dispatch. Does not mark the canvas modified.
TileState& TileInsert(CanvasState& canvas, TileKey key, TileAtlasCell cell) noexcept;

// A view at `viewLevel` draws its loaded tiles of that level, and a loaded level-0 tile where that level's tile is
// missing or not loaded yet.
[[nodiscard]] bool TileDrawn(const CanvasState& canvas, const TileState& tile, TileMip viewLevel) noexcept;

// Persistently mapped memory of kTileSlotBytes per slot, one buffer per direction (GraphicsTileUploadMemory,
// GraphicsTileDownloadMemory), so streaming never waits for the GPU. Slots move tiles of every canvas, so each fits
// a tile of the widest format.
using TileTransferSlot = std::uint8_t;
constexpr TileTransferSlot kTileTransferSlots = 32;
constexpr std::size_t kTileSlotBytes = std::size_t{kTileSize} * kTileSize * std::ranges::max(kPixelFormatTexelBytes);

// Upload: Job (a reader fills the memory) -> Ready (waits for an atlas cell) -> Record -> Gpu -> Free.
// Download: Record -> Gpu -> Job (the writer stores the memory in the cache) -> Free.
// Record: the next submit records the copy between the memory and `cell`. Gpu: recorded, the memory is the
// GPU's until the queue passes `fence`.
enum struct TileSlotState : std::uint8_t {
    Free,
    Job,
    Ready,
    Record,
    Gpu,
};

// Owned by the main thread. A slot whose tile was deleted while the GPU or a job held it finishes and is
// dropped; kCanvasInvalid marks one whose canvas was destroyed.
struct TileSlot {
    std::uint64_t fence;
    TileKey key;
    TileAtlasCell cell;
    Canvas canvas;
    TileSlotState state;
};

struct TileTransfersState {
    std::array<TileSlot, kTileTransferSlots> uploads;
    std::array<TileSlot, kTileTransferSlots> downloads;
};

// Loaded, not visible this frame, persisted, with no transfer in flight and not in the stroke.
[[nodiscard]] bool TileEvictable(Canvas canvas, const TileState& tile) noexcept;
// Frees its cell; the GPU may still read it in a frame in flight, its next fill is recorded after that frame.
void TileEvict(TileState& tile) noexcept;

} // namespace Midori
