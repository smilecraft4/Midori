
cbuffer ViewportCB : register(b0, space1) {
    float4x4 proj;
    float4x4 view;
};

cbuffer TileCB : register(b1, space1) {
    float2 position;
    float2 size;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

static const float2 vertices[4] = {
    float2(0.0, 1.0),
    float2(0.0, 0.0),
    float2(1.0, 1.0),
    float2(1.0, 0.0)
};

static const float2 uvs[4] = {
    float2(0.0, 1.0),
    float2(0.0, 0.0),
    float2(1.0, 1.0),
    float2(1.0, 0.0)
};

VSOutput main(uint vertexID : SV_VertexID) {
    VSOutput output;

    float2 localPos = vertices[vertexID];
    float2 worldPos = (localPos + position) * size;
    float4 clipPos = mul(proj, mul(view, float4(worldPos, 0.0, 1.0)));

    output.position = clipPos;
    output.uv = uvs[vertexID];

    return output;
}