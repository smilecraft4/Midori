#pragma once

#include "midori/base.hpp"
#include "midori/canvas.hpp"
#include "midori/color.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

namespace Midori {

using TileKey = std::uint64_t;
constexpr std::size_t kTileXBitSize = 25;
constexpr std::size_t kTileXBitOffset = 0;
constexpr std::size_t kTileYBitSize = 25;
constexpr std::size_t kTileYBitOffset = kTileXBitSize;
constexpr std::size_t kTileMipBitSize = 2;
constexpr std::size_t kTileMipBitOffset = kTileYBitOffset + kTileYBitSize;
constexpr std::size_t kTileLayerBitSize = 12;
constexpr std::size_t kTileLayerBitOffset = kTileMipBitOffset + kTileMipBitSize;
static_assert((kTileLayerBitOffset + kTileLayerBitSize) <= sizeof(TileKey) * 8);

using TileAxis = std::int32_t;

using TileMip = std::uint8_t;
constexpr TileMip kTileMipMax = 0b11 + 1; // 256, 128, 64, 32

struct Tile {
    Canvas canvas{kCanvasInvalid};
    TileMip mip{};
    Layer layer{kLayerInvalid};
    TileAxis x{};
    TileAxis y{};
};

struct TileNewInfo {
    Tile tile;
    PixelFormat pixelFormat;
    bool initializeTexture;
};
[[nodiscard]] Result<void> TileNew(TileNewInfo newInfo) noexcept;

[[nodiscard]] Result<void> TileLoad(Tile tile) noexcept;

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

[[nodiscard]] Result<void> TileCache(Tile tile) noexcept;

[[nodiscard]] Result<void> TileSave(Tile tile) noexcept;

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
