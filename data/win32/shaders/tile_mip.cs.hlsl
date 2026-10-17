#include "tile_ops.hlsli"

// Builds mip + 1 from mip with a 2x2 box filter; premultiplied, so averaging is correct.
[RootSignature(TILE_OPS_ROOT_SIGNATURE)]
[numthreads(kTileOpsGroupSize, kTileOpsGroupSize, 1)]
void CSMain(uint2 id : SV_DispatchThreadID) {
    const uint2 p = id * 2;
    const float4 sum = dstMips[mip][uint3(p, dstSlice)] + dstMips[mip][uint3(p + uint2(1, 0), dstSlice)]
                     + dstMips[mip][uint3(p + uint2(0, 1), dstSlice)] + dstMips[mip][uint3(p + uint2(1, 1), dstSlice)];
    dstMips[mip + 1][uint3(id, dstSlice)] = sum * 0.25;
}
