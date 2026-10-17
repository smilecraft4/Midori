#include "common.hlsli"

cbuffer TileDraw : register(b1) {
    uint tileSlice;
};

Texture2DArray<float4> tilePool : register(t0);
SamplerState tileSampler : register(s0);

float4 PSMain(TileVertex input) : SV_Target {
    return tilePool.Sample(tileSampler, float3(input.uv, tileSlice));
}
