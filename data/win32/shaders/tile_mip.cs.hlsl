#include "atlas.hlsli"

// Builds a level-k tile from its level-0 tile, each texel the box average of 2^k x 2^k; premultiplied, so the
// average is correct. One UAV per atlas, volatile like the tile pipeline's SRVs.
#define TILE_MIP_ROOT_SIGNATURE                                                                        \
    "RootConstants(num32BitConstants = 2, b0),"                                                        \
    "DescriptorTable(UAV(u0, numDescriptors = 16, flags = DESCRIPTORS_VOLATILE))"

static const uint kTileMipGroupSize = 8;

cbuffer TileMip : register(b0) {
    uint srcCell; // level 0
    uint dstCell;
};

RWTexture2DArray<float4> tilePools[kTileAtlasMax] : register(u0);

[RootSignature(TILE_MIP_ROOT_SIGNATURE)]
[numthreads(kTileMipGroupSize, kTileMipGroupSize, 1)]
void CSMain(uint2 id : SV_DispatchThreadID) {
    if (any(id >= TileAtlasCellSize(dstCell))) {
        return;
    }
    const uint srcPage = TileAtlasCellPage(srcCell);
    const uint dstPage = TileAtlasCellPage(dstCell);
    const uint scale = 1u << TileAtlasCellLevel(dstCell);
    const uint2 src = TileAtlasCellOrigin(srcCell) + id * scale;
    float4 sum = 0.0;
    for (uint y = 0; y < scale; ++y) {
        for (uint x = 0; x < scale; ++x) {
            sum += tilePools[TileAtlasPageAtlas(srcPage)][uint3(src + uint2(x, y), TileAtlasPageSlice(srcPage))];
        }
    }
    tilePools[TileAtlasPageAtlas(dstPage)][uint3(TileAtlasCellOrigin(dstCell) + id, TileAtlasPageSlice(dstPage))] =
        sum / float(scale * scale);
}
