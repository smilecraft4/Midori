#include "atlas.hlsli"

// One UAV per atlas, volatile like the tile pipeline's SRVs.
#define TRANSFORM_ROOT_SIGNATURE                                                                       \
    "RootConstants(num32BitConstants = 21, b0),"                                                       \
    "SRV(t0),"                                                                                         \
    "DescriptorTable(UAV(u0, numDescriptors = 16, flags = DESCRIPTORS_VOLATILE))"

static const uint kTransformGroupSize = 8;
// A lifted grid position without that tile.
static const uint kTransformPageNone = 0xFFFFFFFF;

// Canvas units from the stroke's origin tile. Each float3 is padded by the uint after it.
cbuffer Transform : register(b0) {
    // Where a texel center takes its pixels from, projectively: q = (dot(inverseX, p), dot(inverseY, p),
    // dot(inverseW, p)) with p = (pos, 1), then q.xy / q.z; nothing where q.z is not above 0.
    float3 inverseX;
    uint dstPage;
    float3 inverseY;
    uint srcPage;     // the tile before the transform
    float3 inverseW;
    uint selection;   // 1: the tile is the selection's, which moves alone
    int2 tileGridPos;
    int2 tableOrigin; // the first lifted grid position
    uint2 tableSize;
    uint2 rectOrigin;
    uint restore;     // 1: the transform puts everything back where it was
};

// Per lifted grid position, row by row: the backup pages of the layer's tile and of the selection's.
StructuredBuffer<uint2> table : register(t0);
RWTexture2DArray<float4> tilePools[kTileAtlasMax] : register(u0);

float4 Load(uint page, uint2 texel) {
    return tilePools[TileAtlasPageAtlas(page)][uint3(texel, TileAtlasPageSlice(page))];
}

uint2 Pages(int2 texel) {
    const int2 grid = (texel >> 8) - tableOrigin;
    if (any(grid < 0) || any(grid >= int2(tableSize))) {
        return uint2(kTransformPageNone, kTransformPageNone);
    }
    return table[uint(grid.y) * tableSize.x + uint(grid.x)];
}

// What the transform carries from the texel: the layer's pixel times the selection there, or the selection's own.
float4 Lifted(int2 texel) {
    const uint2 pages = Pages(texel);
    if (pages.y == kTransformPageNone) {
        return 0.0;
    }
    const uint2 local = uint2(texel & 255);
    const float4 selected = Load(pages.y, local);
    if (selection != 0) {
        return selected;
    }
    return pages.x == kTransformPageNone ? 0.0 : Load(pages.x, local) * selected.a;
}

float Selected(int2 texel) {
    const uint2 pages = Pages(texel);
    return pages.y == kTransformPageNone ? 0.0 : Load(pages.y, uint2(texel & 255)).a;
}

// What the selection leaves behind, under the lifted pixels filtered bilinearly where the transform puts them.
[RootSignature(TRANSFORM_ROOT_SIGNATURE)]
[numthreads(kTransformGroupSize, kTransformGroupSize, 1)]
void CSMain(uint2 id : SV_DispatchThreadID) {
    const uint2 texel = rectOrigin + id;
    if (any(texel >= kTileTexels)) {
        return;
    }
    const float4 before = Load(srcPage, texel);
    float4 result = before;
    if (restore == 0) {
        const int2 at = tileGridPos * int(kTileTexels) + int2(texel);
        const float3 pos = float3(float2(at) + 0.5, 1.0);
        const float3 q = float3(dot(inverseX, pos), dot(inverseY, pos), dot(inverseW, pos));
        float4 lifted = float4(0.0, 0.0, 0.0, 0.0);
        if (q.z > 0.0) {
            const float2 from = q.xy / q.z - 0.5;
            const int2 base = int2(floor(from));
            const float2 f = from - float2(base);
            lifted = lerp(lerp(Lifted(base), Lifted(base + int2(1, 0)), f.x),
                          lerp(Lifted(base + int2(0, 1)), Lifted(base + int2(1, 1)), f.x), f.y);
        }
        const float4 left = selection != 0 ? float4(0.0, 0.0, 0.0, 0.0) : before * (1.0 - Selected(at));
        result = lifted + left * (1.0 - lifted.a);
    }
    tilePools[TileAtlasPageAtlas(dstPage)][uint3(texel, TileAtlasPageSlice(dstPage))] = result;
}
