// The triangle lives here: positions/colors come from SV_VertexID, so no vertex buffer is bound.
#include "common.hlsli"

static const float2 kPositions[3] = { float2(0.0, 0.6), float2(0.6, -0.6), float2(-0.6, -0.6) };
static const float4 kColors[3] = { float4(1, 0, 0, 1), float4(0, 1, 0, 1), float4(0, 0, 1, 1) };

ColorVertex VSMain(uint vertexID : SV_VertexID) {
    ColorVertex output;
    output.position = float4(kPositions[vertexID], 0.0, 1.0);
    output.color = kColors[vertexID];
    return output;
}
