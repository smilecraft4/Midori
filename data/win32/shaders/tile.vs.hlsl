#include "common.hlsli"

// One triangle over the whole window; the pixel shader finds the tiles itself.
[RootSignature(TILE_ROOT_SIGNATURE)]
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
