#ifndef MIDORI_ATLAS_HLSLI
#define MIDORI_ATLAS_HLSLI

// Mirrors the tile atlas constants and cell encoding in canvas.hpp. The root signatures spell kTileAtlasMax out as
// their numDescriptors.
static const uint kTileAtlasMax = 16;
static const uint kTileAtlasPages = 64;
static const uint kTileAtlasBlockSize = 32;
static const uint kTileTexels = 256;

uint TileAtlasPageAtlas(uint page) {
    return page / kTileAtlasPages;
}

uint TileAtlasPageSlice(uint page) {
    return page % kTileAtlasPages;
}

// A cell is page << 8 | Morton index of its first block << 2 | level.
uint TileAtlasCellPage(uint cell) {
    return cell >> 8;
}

uint TileAtlasCellLevel(uint cell) {
    return cell & 3;
}

uint TileAtlasCellSize(uint cell) {
    return kTileTexels >> TileAtlasCellLevel(cell);
}

// Texel origin in its page.
uint2 TileAtlasCellOrigin(uint cell) {
    const uint i = (cell >> 2) & 63;
    const uint x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4);
    const uint y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4);
    return uint2(x, y) * kTileAtlasBlockSize;
}

#endif
