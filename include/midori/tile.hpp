#pragma once

#include "midori/base.hpp"
#include "midori/canvas.hpp"
#include "midori/color.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

namespace Midori {

using TileAxis = std::int32_t;

using TileMip = std::uint8_t;
constexpr TileMip kTileMipMax = 0b11 + 1; // 256, 128, 64, 32

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

constexpr TileKey TileGridPosKey(TileKey key) noexcept {
    return key & ~kTileLayerMask;
}

constexpr TileKey TileGridPosLayerKey(TileKey gridPosKey, Layer layer) noexcept {
    return gridPosKey | (static_cast<TileKey>(layer) << kTileLayerBitOffset);
}

constexpr TileMip TileKeyMip(TileKey key) noexcept {
    return static_cast<TileMip>((key >> kTileMipBitOffset) & ((TileKey{1} << kTileMipBitSize) - 1));
}

struct TileDuplicateInfo {
    Tile srcTile;
    Tile dstTile;
    BlendMode blendMode;
    float opacity;
};
[[nodiscard]] Result<void> TileDuplicate(TileDuplicateInfo duplicateInfo) noexcept;

struct TileMergeInfo {
    Tile belowTile;
    Tile overTile;
    BlendMode blendMode;
    float opacity;
};
[[nodiscard]] Result<void> TileMerge(TileMergeInfo mergeInfo) noexcept;

struct TileMoveInfo {
    Tile srcTile;
    Tile dstTile;
    BlendMode blendMode;
    float opacity;
};

[[nodiscard]] Result<void> TileMove(TileMoveInfo moveInfo) noexcept;

[[nodiscard]] Result<void> TileUnload(Tile tile) noexcept;

[[nodiscard]] Result<void> TileDelete(Tile tile) noexcept;

using TileTextureTransferSlot = std::uint8_t;

[[nodiscard]] Result<void> TileTextureUploadsUpdate() noexcept;
[[nodiscard]] Result<TileTextureTransferSlot> TileTextureUploadSlotReserve(Tile tile) noexcept;
[[nodiscard]] Result<void> TileTextureUploadSlotFree(TileTextureTransferSlot slot) noexcept;
[[nodiscard]] Result<void> TileTextureUploadSlotSubmit(TileTextureTransferSlot slot) noexcept;
[[nodiscard]] Result<std::span<std::byte>> TileTextureUploadSlotData(TileTextureTransferSlot slot) noexcept;

[[nodiscard]] Result<void> TileTextureDownloadsUpdate() noexcept;
[[nodiscard]] Result<TileTextureTransferSlot> TileTextureDownloadSlotReserve(Tile tile) noexcept;
[[nodiscard]] Result<void> TileTextureDownloadSlotFree(TileTextureTransferSlot slot) noexcept;
[[nodiscard]] Result<void> TileTextureDownloadSlotSubmit(TileTextureTransferSlot slot) noexcept;
[[nodiscard]] Result<std::span<const std::byte>> TileTextureDownloadSlotData(TileTextureTransferSlot slot) noexcept;

} // namespace Midori
