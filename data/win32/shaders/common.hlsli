#ifndef MIDORI_COMMON_HLSLI
#define MIDORI_COMMON_HLSLI

// Root signature of the tile pipeline, embedded in tile.vs.hlsl and created from its bytecode. One SRV per atlas;
// volatile, so the atlases not created yet may stay null and new ones may be written while a frame is in flight.
#define TILE_ROOT_SIGNATURE                                                                            \
    "RootConstants(num32BitConstants = 32, b0, visibility = SHADER_VISIBILITY_VERTEX),"                \
    "RootConstants(num32BitConstants = 11, b1),"                                                       \
    "DescriptorTable(SRV(t0, numDescriptors = 16, flags = DESCRIPTORS_VOLATILE),"                      \
    " visibility = SHADER_VISIBILITY_PIXEL)"

cbuffer Viewport : register(b0) {
    row_major float4x4 canvasToView;
    row_major float4x4 viewToClip;
};

cbuffer TileDraw : register(b1) {
    uint4 tileNeighboursTop;    // the cells beside the tile: top left, top, top right, left
    uint4 tileNeighboursBottom; // right, bottom left, bottom, bottom right
    int2 tileGridPos;           // relative to the view's tile, see canvasToView
    uint tileCell;
};

struct TileVertex {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD;
};

#endif
