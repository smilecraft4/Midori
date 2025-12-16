#version 460

// Optimize for tile only
layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;


layout(set = 2, binding = 0) uniform Stroke {
    uint stroke_points_num;
};

layout(set = 2, binding = 1) uniform Tile {
    vec2 tile_pos;
    vec2 tile_size;
};

struct StrokePointData {
    vec4 color;
    vec2 position;
    float radius;
    float flow;
    float hardness;
    float pad0;
    float pad1;
    float pad2;
};

layout(set = 0, binding = 0) uniform sampler2D tex_alpha;

layout(std430, set = 0, binding = 1) readonly buffer StrokePointsBuffer {
    StrokePointData stroke_points[];
};

layout(set = 1, binding = 0, rgba8) uniform image2D tile_tex;

vec3 sRGBToLinear(vec3 rgb) {
  // See https://gamedev.stackexchange.com/questions/92015/optimized-linear-to-srgb-glsl
  return mix(pow((rgb + 0.055) * (1.0 / 1.055), vec3(2.4)),
             rgb * (1.0/12.92),
             lessThanEqual(rgb, vec3(0.04045)));
}

layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;
void main() {
    ivec2 tex_coord = ivec2(gl_GlobalInvocationID.xy);
    const vec2 tile_position = vec2(tile_pos * tile_size) + vec2(tex_coord);

    for(int i = 0; i < stroke_points_num; i++) {
        const float d = distance(tile_position, stroke_points[i].position);
        if(d <= stroke_points[i].radius) {
            vec2 brushAlphaUV = (tile_position - stroke_points[i].position) / (2.0 * stroke_points[i].radius) + vec2(0.5);
            float mask = texture(tex_alpha, brushAlphaUV).r;
            mask = clamp(smoothstep(0.0, max(0.01, 1.0 - stroke_points[i].hardness), mask), 0.0, 1.0);


            vec4 dstColor = imageLoad(tile_tex, tex_coord);
            if(dstColor.a == 1) {
                continue;
            }

            vec4 srcColor = stroke_points[i].color;
            srcColor.rgb = sRGBToLinear(srcColor.rgb);

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
            
            imageStore(tile_tex, tex_coord, dstColor);
        }
    }
}