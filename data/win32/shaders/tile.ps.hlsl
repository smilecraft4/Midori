#include "atlas.hlsli"
#include "common.hlsli"

Texture2DArray<float4> tilePools[kTileAtlasMax] : register(t0);

// The cell drawn at a tile, from the origin tile; kTileAtlasCellInvalid outside the page table.
uint TileCell(int2 tile) {
    const int2 local = tile - regionOrigin;
    if (any(local < 0) || any(local >= int2(regionSize))) {
        return kTileAtlasCellInvalid;
    }
    return pageTable[local.y * regionSize.x + local.x];
}

// A texel of the view level's grid, which runs on across the tiles, so filtering never sees an edge. A tile shown from
// level 0 while its own level is missing gives the level 0 texel at the center of the footprint.
float4 LevelTexel(int2 texel) {
    const uint shift = 8 - level; // texels per tile: 256 >> level
    const uint cell = TileCell(texel >> shift);
    if (cell == kTileAtlasCellInvalid) {
        return 0.0;
    }
    const uint2 local = uint2(texel & int((1u << shift) - 1u));
    const uint scale = 1u << (level - TileAtlasCellLevel(cell));
    const uint2 pos = TileAtlasCellOrigin(cell) + local * scale + scale / 2;
    const uint page = TileAtlasCellPage(cell);
    return tilePools[NonUniformResourceIndex(TileAtlasPageAtlas(page))].Load(int4(pos, TileAtlasPageSlice(page), 0));
}

// Bilinear by hand over the premultiplied texels.
float4 PSMain(float4 position : SV_Position) : SV_Target {
    const float2 canvas =
        float2(dot(pixelToCanvasX, position.xy), dot(pixelToCanvasY, position.xy)) + canvasOffset;
    const float2 pos = canvas / float(1u << level) - 0.5;
    const int2 base = int2(floor(pos));
    const float2 f = pos - float2(base);
    const float4 top = lerp(LevelTexel(base), LevelTexel(base + int2(1, 0)), f.x);
    const float4 bottom = lerp(LevelTexel(base + int2(0, 1)), LevelTexel(base + int2(1, 1)), f.x);
    return lerp(top, bottom, f.y);
}
