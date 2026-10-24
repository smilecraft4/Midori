#include "atlas.hlsli"
#include "blend.hlsli"

// One UAV per atlas, volatile like the tile pipeline's SRVs.
#define STROKE_ROOT_SIGNATURE                                                                          \
    "RootConstants(num32BitConstants = 15, b0),"                                                       \
    "SRV(t0),"                                                                                         \
    "DescriptorTable(UAV(u0, numDescriptors = 16, flags = DESCRIPTORS_VOLATILE))"

static const uint kStrokeGroupSize = 8;
// StrokeMode in stroke.hpp.
static const uint kStrokePaint = 0;
static const uint kStrokeErase = 1;
static const uint kStrokeReplace = 2;
static const uint kStrokeIntersect = 3;
static const uint kStrokeExclude = 4;
// maskPage: nothing selected anywhere, so nothing clips; or nothing selected in this tile.
static const uint kStrokeMaskNone = 0xFFFFFFFF;
static const uint kStrokeMaskEmpty = 0xFFFFFFFE;

// StrokeDab in stroke.hpp.
struct StrokeDab {
    float2 pos;
    float radius;
    float hardness;
    float flow;
    float opacity;
    float width; // a rectangle from pos when above 0
    float height;
};

cbuffer Stroke : register(b0) {
    float4 color; // premultiplied; Erase: how much of each channel goes
    int2 tileGridPos; // relative to the stroke's origin tile, like the dabs
    uint2 rectOrigin;
    uint dstPage;
    uint srcPage; // the tile before the stroke
    uint dabFirst;
    uint dabEnd;
    uint blendMode;
    uint mode;
    uint maskPage; // the selection's mask at this tile, coverage in alpha
};

StructuredBuffer<StrokeDab> dabs : register(t0);
RWTexture2DArray<float4> tilePools[kTileAtlasMax] : register(u0);

float DabMask(StrokeDab dab, float2 pos) {
    if (dab.width > 0.0) {
        // How much of the texel around `pos` the rectangle covers, along each axis.
        const float2 cover = saturate(min(pos + 0.5, dab.pos + float2(dab.width, dab.height))
                                      - max(pos - 0.5, dab.pos));
        return cover.x * cover.y;
    }
    const float edge = max((1.0 - dab.hardness) * dab.radius, 1.0);
    return smoothstep(0.0, 1.0, (dab.radius - distance(pos, dab.pos)) / edge);
}

// Alpha only goes as far as every channel does, so premultiplied colors stay within it.
float4 Erase(float4 dst, float3 amount) {
    return float4(dst.rgb * (1.0 - amount), dst.a * (1.0 - min(amount.r, min(amount.g, amount.b))));
}

[RootSignature(STROKE_ROOT_SIGNATURE)]
[numthreads(kStrokeGroupSize, kStrokeGroupSize, 1)]
void CSMain(uint2 id : SV_DispatchThreadID) {
    const uint2 texel = rectOrigin + id;
    if (any(texel >= kTileTexels)) {
        return;
    }
    const float2 pos = float2(tileGridPos) * kTileTexels + float2(texel) + 0.5;

    // Each dab pulls the coverage toward its opacity by its flow and never past it, so a constant opacity
    // caps the stroke like Photoshop's and a varying one only ever darkens.
    float coverage = 0.0;
    for (uint i = dabFirst; i < dabEnd; ++i) {
        const StrokeDab dab = dabs[i];
        if (coverage < dab.opacity) {
            coverage = lerp(coverage, dab.opacity, DabMask(dab, pos) * dab.flow);
        }
    }
    if (maskPage == kStrokeMaskEmpty) {
        coverage = 0.0;
    } else if (maskPage != kStrokeMaskNone) {
        coverage *= tilePools[TileAtlasPageAtlas(maskPage)][uint3(texel, TileAtlasPageSlice(maskPage))].a;
    }
    const float4 below = tilePools[TileAtlasPageAtlas(srcPage)][uint3(texel, TileAtlasPageSlice(srcPage))];
    const float4 src = color * coverage;
    float4 result = Blend(below, src, blendMode);
    if (mode == kStrokeErase) {
        result = Erase(below, src.rgb);
    } else if (mode == kStrokeReplace) {
        result = src;
    } else if (mode == kStrokeIntersect) {
        result = below * coverage;
    } else if (mode == kStrokeExclude) {
        result = below + src - 2.0 * below * src;
    }
    tilePools[TileAtlasPageAtlas(dstPage)][uint3(texel, TileAtlasPageSlice(dstPage))] = result;
}
