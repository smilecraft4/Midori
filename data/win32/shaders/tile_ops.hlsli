#ifndef MIDORI_TILE_OPS_HLSLI
#define MIDORI_TILE_OPS_HLSLI

// Tiles of one pixel format share a Texture2DArray, one slice per tile, kept in UNORDERED_ACCESS
// for compute. Each table holds one UAV per mip.
#define TILE_OPS_ROOT_SIGNATURE                                                                        \
    "RootConstants(num32BitConstants = 5, b0),"                                                        \
    "DescriptorTable(UAV(u0, numDescriptors = 4, space = 0)),"                                         \
    "DescriptorTable(UAV(u0, numDescriptors = 4, space = 1))"

static const uint kTileOpsGroupSize = 8;

cbuffer TileOp : register(b0) {
    uint srcSlice;
    uint dstSlice;
    uint blendMode; // BlendMode in color.hpp
    float opacity;
    uint mip;
};

RWTexture2DArray<float4> dstMips[4] : register(u0, space0);
RWTexture2DArray<float4> srcMips[4] : register(u0, space1);

#endif
