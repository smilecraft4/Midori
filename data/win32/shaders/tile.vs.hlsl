// A tile is a fixed 256x256 screen-pixel quad: the vertex shader maps pixel coordinates to NDC with
// the current viewport, so resizing the window never changes the tile's on-screen size.
#include "common.hlsli"

// D3D11 constant buffers are sized in 16-byte rows. The padding is spelled out so the C++ mirror,
// struct { float viewportSize[2]; float padding[2]; }, is obviously 16 bytes too.
cbuffer Constants : register(b0) {
    float2 viewportSize;
    float2 padding;
};

static const float kTileSize = 256.0;

// File-scope static const: fxc reads these from an immediate constant buffer instead of rebuilding
// an indexable temp array on every vertex.
static const float2 kPixelOffsets[6] = {
    float2(0.0, 0.0), float2(kTileSize, 0.0), float2(kTileSize, kTileSize),
    float2(0.0, 0.0), float2(kTileSize, kTileSize), float2(0.0, kTileSize),
};
static const float4 kColors[6] = {
    float4(1, 0, 0, 1), float4(0, 1, 0, 1), float4(0, 0, 1, 1),
    float4(1, 0, 0, 1), float4(0, 0, 1, 1), float4(1, 1, 1, 1),
};

ColorVertex VSMain(uint vertexID : SV_VertexID) {
    const float2 pixel = kPixelOffsets[vertexID];
    ColorVertex output;
    output.position = float4(pixel.x / viewportSize.x * 2.0 - 1.0,
                             1.0 - pixel.y / viewportSize.y * 2.0, 0.0, 1.0);
    output.color = kColors[vertexID];
    return output;
}
