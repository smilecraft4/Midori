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
// level 0 while its own level is missing (painting deletes it until the stroke ends) averages the texel's footprint,
// the same box tile_mip.cs.hlsl builds the level with, so nothing changes once it is built.
float4 LevelTexel(int2 texel) {
    const uint shift = 8 - level; // texels per tile: 256 >> level
    const uint cell = TileCell(texel >> shift);
    if (cell == kTileAtlasCellInvalid) {
        return 0.0;
    }
    const uint2 local = uint2(texel & int((1u << shift) - 1u));
    const uint scale = 1u << (level - TileAtlasCellLevel(cell));
    const uint2 origin = TileAtlasCellOrigin(cell) + local * scale;
    const uint page = TileAtlasCellPage(cell);
    const uint atlas = TileAtlasPageAtlas(page);
    const uint slice = TileAtlasPageSlice(page);
    float4 sum = 0.0;
    [loop]
    for (uint y = 0; y < scale; ++y) {
        [loop]
        for (uint x = 0; x < scale; ++x) {
            sum += tilePools[NonUniformResourceIndex(atlas)].Load(int4(origin + uint2(x, y), slice, 0));
        }
    }
    return sum / float(scale * scale);
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
