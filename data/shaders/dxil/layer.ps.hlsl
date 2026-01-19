struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    nointerpolation float opacity : TEXCOORD1;
    nointerpolation uint blend_mode : TEXCOORD2;
};

Texture2D<float4> layer_texture : register(t0, space2);
SamplerState linearSampler : register(s0, space2);

float4 main(PSInput input) : SV_Target0 {
    float4 layer_color = layer_texture.Sample(linearSampler, input.uv);
    // layer_color.rgb *= layer_color.a;
    
    return layer_color;
}