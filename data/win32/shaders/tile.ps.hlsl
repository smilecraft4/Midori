#include "atlas.hlsli"
#include "common.hlsli"

Texture2DArray<float4> tilePools[kTileAtlasMax] : register(t0);
SamplerState tileSampler : register(s0);

// A cell smaller than its page has other tiles' texels around it: the bilinear footprint stays half a texel inside.
float4 PSMain(TileVertex input) : SV_Target {
    const uint page = TileAtlasCellPage(tileCell);
    const float size = TileAtlasCellSize(tileCell);
    const float2 texel = clamp(input.uv * size, 0.5, size - 0.5) + TileAtlasCellOrigin(tileCell);
    return tilePools[TileAtlasPageAtlas(page)].Sample(tileSampler,
                                                      float3(texel / kTileTexels, TileAtlasPageSlice(page)));
}
