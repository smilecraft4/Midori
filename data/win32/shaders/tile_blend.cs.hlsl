#include "blend.hlsli"
#include "tile_ops.hlsli"

[RootSignature(TILE_OPS_ROOT_SIGNATURE)]
[numthreads(kTileOpsGroupSize, kTileOpsGroupSize, 1)]
void CSMain(uint2 id : SV_DispatchThreadID) {
    const float4 src = srcMips[0][uint3(id, srcSlice)] * opacity;
    const float4 dst = dstMips[0][uint3(id, dstSlice)];
    dstMips[0][uint3(id, dstSlice)] = Blend(dst, src, blendMode);
}
