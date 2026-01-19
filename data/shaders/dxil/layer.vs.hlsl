cbuffer LayerCB : register(b0, space1) {
    float opacity;
    uint blend_mode;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    nointerpolation float opacity_out : TEXCOORD1;
    nointerpolation uint blend_mode_out : TEXCOORD2;
};

static const float2 positions[3] = {
    float2(-1.0f, -1.0f),
    float2( 3.0f, -1.0f),
    float2(-1.0f,  3.0f) 
};

static const float2 uvs[3] = {
    float2(0.0f, 0.0f),
    float2(2.0f, 0.0f),
    float2(0.0f, 2.0f)
};

VSOutput main(uint vertexID : SV_VertexID) {
    VSOutput output;

    float2 pos = positions[vertexID];
    output.uv = uvs[vertexID];

    output.opacity_out = opacity;
    output.blend_mode_out = blend_mode;

    output.position = float4(pos, 0.0f, 1.0f);
    return output;
}