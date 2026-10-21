#include "common.hlsli"

static const float kTileSize = 256.0;

[RootSignature(TILE_ROOT_SIGNATURE)]
TileVertex VSMain(uint id : SV_VertexID) {
    const float2 uv = float2(id & 1, id >> 1);
    const float4 view = mul(float4((float2(tileGridPos) + uv) * kTileSize, 0.0, 1.0), canvasToView);

    TileVertex output;
    output.position = mul(view, viewToClip);
    output.uv = uv;
    return output;
}
