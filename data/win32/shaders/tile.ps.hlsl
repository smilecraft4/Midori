#include "atlas.hlsli"
#include "common.hlsli"

Texture2DArray<float4> tilePools[kTileAtlasMax] : register(t0);

// A texel of the tile, or past its edge of the tile beside it, so both sides of an edge filter the same texels. A
// neighbour not loaded clamps to the tile's own edge instead.
float4 TileTexel(int2 texel, int size) {
    const int2 side = int2(texel >= size) - int2(texel < 0);
    uint cell = tileCell;
    if (any(side != 0)) {
        const uint index = uint((side.y + 1) * 3 + (side.x + 1));
        const uint neighbour = index < 4 ? tileNeighboursTop[index] : tileNeighboursBottom[index - 5];
        if (neighbour == kTileAtlasCellInvalid) {
            texel = clamp(texel, 0, size - 1);
        } else {
            cell = neighbour;
            texel -= side * size;
        }
    }
    const uint page = TileAtlasCellPage(cell);
    const uint2 pos = TileAtlasCellOrigin(cell) + uint2(texel);
    return tilePools[NonUniformResourceIndex(TileAtlasPageAtlas(page))].Load(int4(pos, TileAtlasPageSlice(page), 0));
}

// Bilinear by hand, the premultiplied texels weighted like the hardware filter would.
float4 PSMain(TileVertex input) : SV_Target {
    const int size = int(TileAtlasCellSize(tileCell));
    const float2 pos = input.uv * size - 0.5;
    const int2 base = int2(floor(pos));
    const float2 f = pos - float2(base);
    const float4 top = lerp(TileTexel(base, size), TileTexel(base + int2(1, 0), size), f.x);
    const float4 bottom = lerp(TileTexel(base + int2(0, 1), size), TileTexel(base + int2(1, 1), size), f.x);
    return lerp(top, bottom, f.y);
}
