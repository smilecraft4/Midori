#ifndef MIDORI_COMMON_HLSLI
#define MIDORI_COMMON_HLSLI

// Root signature of the tile pipeline, embedded in tile.vs.hlsl and created from its bytecode.
#define TILE_ROOT_SIGNATURE                                                                            \
    "RootConstants(num32BitConstants = 32, b0, visibility = SHADER_VISIBILITY_VERTEX),"                \
    "RootConstants(num32BitConstants = 1, b1, visibility = SHADER_VISIBILITY_PIXEL),"                  \
    "DescriptorTable(SRV(t0), visibility = SHADER_VISIBILITY_PIXEL),"                                  \
    "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_CLAMP,"          \
    " addressV = TEXTURE_ADDRESS_CLAMP, visibility = SHADER_VISIBILITY_PIXEL)"

cbuffer Viewport : register(b0) {
    row_major float4x4 canvasToView;
    row_major float4x4 viewToClip;
};

struct TileVertex {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD;
};

#endif
