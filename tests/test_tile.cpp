#include "test.hpp"

#include "midori/canvas.hpp"
#include "midori/tile.hpp"

#include <array>

namespace Midori {

void TestTileKeys(TestContext& t) noexcept {
    constexpr TileAxis kAxisMin = -(TileAxis{1} << (kTileXBitSize - 1));
    constexpr TileAxis kAxisMax = (TileAxis{1} << (kTileXBitSize - 1)) - 1;
    constexpr std::array kPositions = {
        TileGridPos{.x = 0, .y = 0},
        TileGridPos{.x = -1, .y = -1},
        TileGridPos{.x = 123, .y = -4567},
        TileGridPos{.x = kAxisMin, .y = kAxisMax},
        TileGridPos{.x = kAxisMax, .y = kAxisMin},
    };
    constexpr std::array kLayers = {Layer{0}, Layer{1}, static_cast<Layer>(kLayerMax - 1)};
    for (const TileGridPos pos : kPositions) {
        for (const Layer layer : kLayers) {
            for (TileMip mip = 0; mip < kTileMipMax; ++mip) {
                const TileKey key = TileKeyMake(layer, mip, pos);
                MIDORI_CHECK(t, TileKeyGridPos(key).x == pos.x && TileKeyGridPos(key).y == pos.y);
                MIDORI_CHECK(t, TileKeyLayer(key) == layer);
                MIDORI_CHECK(t, TileKeyMip(key) == mip);
                MIDORI_CHECK(t, TileKeyWithMip(key, 2) == TileKeyMake(layer, 2, pos));
            }
            const TileKey gridPos = TileGridPosKey(TileKeyMake(layer, 0, pos));
            MIDORI_CHECK(t, gridPos == TileKeyMake(0, 0, pos));
            MIDORI_CHECK(t, TileGridPosLayerKey(gridPos, 7) == TileKeyMake(7, 0, pos));
        }
    }

    // Layer, then level, then row, then column, with -1 right before 0.
    MIDORI_CHECK(t, TileKeyMake(0, 0, {.x = -1, .y = 0}) < TileKeyMake(0, 0, {.x = 0, .y = 0}));
    MIDORI_CHECK(t, TileKeyMake(0, 0, {.x = kAxisMax, .y = -1}) < TileKeyMake(0, 0, {.x = kAxisMin, .y = 0}));
    MIDORI_CHECK(t,
                 TileKeyMake(0, 0, {.x = kAxisMax, .y = kAxisMax}) < TileKeyMake(0, 1, {.x = kAxisMin, .y = kAxisMin}));
    MIDORI_CHECK(t,
                 TileKeyMake(0, 3, {.x = kAxisMax, .y = kAxisMax}) < TileKeyMake(1, 0, {.x = kAxisMin, .y = kAxisMin}));

    MIDORI_CHECK(t, TileLevelSize(0) == 256 && TileLevelSize(3) == 32);
    MIDORI_CHECK(t, TileBytes(PixelFormat::RGBA8) == std::size_t{256} * 256 * 4);
    // Rows pad to the 256 bytes GPU copies need.
    MIDORI_CHECK(t, TileLevelPitch(3, PixelFormat::RGBA8) == 256);
}

void TestAtlasCells(TestContext& t) noexcept {
    for (std::uint32_t i = 0; i < kTileAtlasBlocks; ++i) {
        const std::uint32_t x = TileBlockX(i);
        const std::uint32_t y = TileBlockY(i);
        MIDORI_CHECK(t, x < 8 && y < 8);
        MIDORI_CHECK(t, TileBlockIndex(x, y) == i);
    }
    // Morton order: x takes the even bits.
    MIDORI_CHECK(t, TileBlockX(1) == 1 && TileBlockY(1) == 0);
    MIDORI_CHECK(t, TileBlockX(2) == 0 && TileBlockY(2) == 1);
    MIDORI_CHECK(t, TileBlockX(3) == 1 && TileBlockY(3) == 1);

    const TileAtlasCell cell = TileAtlasCellMake(5, 37, 1);
    MIDORI_CHECK(t, TileAtlasCellPage(cell) == 5);
    MIDORI_CHECK(t, TileAtlasCellBlock(cell) == 37);
    MIDORI_CHECK(t, TileAtlasCellLevel(cell) == 1);
    MIDORI_CHECK(t, TileAtlasCellX(TileAtlasCellMake(0, 1, 0)) == kTileAtlasBlockSize);
    MIDORI_CHECK(t, TileAtlasCellY(TileAtlasCellMake(0, 2, 0)) == kTileAtlasBlockSize);
    MIDORI_CHECK(t, TileAtlasLevelBlocks(0) == 64 && TileAtlasLevelBlocks(1) == 16 && TileAtlasLevelBlocks(3) == 1);
}

} // namespace Midori
