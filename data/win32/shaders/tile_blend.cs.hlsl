#include "atlas.hlsli"
#include "blend.hlsli"

// Composites a folder's level-0 tile from its children's, bottom first, each with its opacity and blend mode, in float
// over transparent. One UAV per atlas, volatile like the tile pipeline's SRVs.
#define TILE_BLEND_ROOT_SIGNATURE                                                                      \
    "RootConstants(num32BitConstants = 3, b0),"                                                        \
    "SRV(t0),"                                                                                         \
    "DescriptorTable(UAV(u0, numDescriptors = 16, flags = DESCRIPTORS_VOLATILE))"

static const uint kTileBlendGroupSize = 8;

// CompositeGpuSource in win32_graphics.hpp.
struct Source {
    uint cell;
    float opacity;
    uint blendMode; // BlendMode in color.hpp
};

// CompositeConstants in win32_graphics.cpp.
cbuffer Composite : register(b0) {
    uint dstCell;
    uint firstSource;
    uint sourceCount;
};

StructuredBuffer<Source> sources : register(t0);
RWTexture2DArray<float4> tilePools[kTileAtlasMax] : register(u0);

[RootSignature(TILE_BLEND_ROOT_SIGNATURE)]
[numthreads(kTileBlendGroupSize, kTileBlendGroupSize, 1)]
void CSMain(uint2 id : SV_DispatchThreadID) {
    float4 color = 0.0;
    for (uint i = 0; i < sourceCount; ++i) {
        const Source source = sources[firstSource + i];
        const uint page = TileAtlasCellPage(source.cell);
        const float4 texel =
            tilePools[TileAtlasPageAtlas(page)][uint3(TileAtlasCellOrigin(source.cell) + id, TileAtlasPageSlice(page))];
        color = Blend(color, texel * source.opacity, source.blendMode);
    }
    const uint page = TileAtlasCellPage(dstCell);
    tilePools[TileAtlasPageAtlas(page)][uint3(TileAtlasCellOrigin(dstCell) + id, TileAtlasPageSlice(page))] = color;
}
