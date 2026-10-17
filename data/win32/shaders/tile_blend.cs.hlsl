#include "tile_ops.hlsli"

static const uint kBlendNormal = 0;
static const uint kBlendMultiply = 1;
static const uint kBlendAdd = 2;
static const uint kBlendScreen = 3;

// Premultiplied src over premultiplied dst, separable blend modes in their premultiplied form.
float4 Blend(float4 dst, float4 src, uint mode) {
    float3 color;
    switch (mode) {
        case kBlendMultiply:
            color = src.rgb * (1.0 - dst.a) + dst.rgb * (1.0 - src.a) + src.rgb * dst.rgb;
            break;
        case kBlendAdd:
            color = src.rgb + dst.rgb;
            break;
        case kBlendScreen:
            color = src.rgb + dst.rgb - src.rgb * dst.rgb;
            break;
        default:
            color = src.rgb + dst.rgb * (1.0 - src.a);
            break;
    }
    return float4(color, src.a + dst.a - src.a * dst.a);
}

[RootSignature(TILE_OPS_ROOT_SIGNATURE)]
[numthreads(kTileOpsGroupSize, kTileOpsGroupSize, 1)]
void CSMain(uint2 id : SV_DispatchThreadID) {
    const float4 src = srcMips[0][uint3(id, srcSlice)] * opacity;
    const float4 dst = dstMips[0][uint3(id, dstSlice)];
    dstMips[0][uint3(id, dstSlice)] = Blend(dst, src, blendMode);
}
