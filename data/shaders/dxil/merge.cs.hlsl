Texture2D<float4> src_tex : register(t0, space0);
SamplerState pointSampler : register(s0, space0);

RWTexture2D<float4> dst_tex : register(u0, space1);

cbuffer MergeCB : register(b0, space2) {
    uint   src_blend_mode;
    float  src_opacity;
    int2   src_start;
    int2   src_size;
    float  dst_opacity;
    uint   dst_blend_mode;
    int2   dst_pos;
    int2   dst_size;
};

[numthreads(32, 32, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
    int2 coord = (int2)dispatchThreadID.xy;

    float4 dstColor = dst_tex[coord];
    // dstColor *= dst_opacity;

    float4 srcColor = src_tex.Load(int3(coord, 0));
    // srcColor *= src_opacity;

    dstColor.rgb = srcColor.rgb + dstColor.rgb * (1.0 - srcColor.a);
    dstColor.a = srcColor.a + dstColor.a * (1.0 - srcColor.a);

    dst_tex[coord] = dstColor;
}