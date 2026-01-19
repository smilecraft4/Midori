
Texture2D<float> tex_alpha : register(t0, space0);
SamplerState linearSampler  : register(s0, space0);

struct StrokePointData {
    float4 color;
    float2 position;
    float radius;
    float flow;
    float hardness;
    float pad0;
    float pad1;
    float pad2;
};
StructuredBuffer<StrokePointData> stroke_points : register(t1, space0);

RWTexture2D<float4> tile_tex : register(u0, space1);

cbuffer StrokeCB : register(b0, space2) {
    uint stroke_points_num;
};

cbuffer TileCB : register(b1, space2) {
    float2 tile_pos;
    float2 tile_size;
};

[numthreads(32, 32, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
    int2 tex_coord = (int2)dispatchThreadID.xy;

    float2 tile_position = tile_pos * tile_size + (float2)tex_coord;

    for (uint i = 0; i < stroke_points_num; i++) {
        float d = distance(tile_position, stroke_points[i].position);
        if (d > stroke_points[i].radius) {
            continue;
        }

        float2 brushAlphaUV = (tile_position - stroke_points[i].position) / (2.0 * stroke_points[i].radius) + 0.5.xx;
        float mask = tex_alpha.SampleLevel(linearSampler, brushAlphaUV, 0).r;

        mask = saturate(smoothstep(0.0, max(0.01, 1.0 - stroke_points[i].hardness), mask));

        float4 dstColor = tile_tex[tex_coord];
        if(dstColor.a == 1) {
            continue;
        }

        float4 srcColor = stroke_points[i].color;

        float alpha = stroke_points[i].flow;
        alpha *= mask;
        if(alpha == 0) {
            continue;
        }

        alpha = (dstColor.a - min(stroke_points[i].color.a, alpha + dstColor.a * (1.0 - alpha))) / (dstColor.a - 1.0);
        if(alpha <= 0) {
            continue;
        }

        dstColor.rgb = (srcColor.rgb * alpha) + dstColor.rgb * (1.0 - alpha);
        dstColor.a = alpha + dstColor.a * (1.0 - alpha);
        
        tile_tex[tex_coord] = dstColor;
    }
}