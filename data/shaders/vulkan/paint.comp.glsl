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

layout(std430, set = 0, binding = 0) readonly buffer StrokePointsBuffer {
    StrokePointData stroke_points[];
};


// layout(set = 0, binding = 0) uniform sampler2D tex_alpha;

layout(set = 1, binding = 0, rgba8) uniform image2D tile_tex;

layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;
void main() {
    ivec2 tex_coord = ivec2(gl_GlobalInvocationID.xy);
    const vec2 tile_position = vec2(tile_pos * tile_size) + vec2(tex_coord);

    for(int i = 0; i < stroke_points_num; i++) {
        const float d = distance(tile_position, stroke_points[i].position);
        if(d <= stroke_points[i].radius) {
            const float mask = 1.0 - smoothstep(stroke_points[i].radius * stroke_points[i].hardness, stroke_points[i].radius, d);

            vec4 dst_color = imageLoad(tile_tex, tex_coord);

            if(dst_color.a <= stroke_points[i].color.a) {
                vec4 src_color = stroke_points[i].color;
                src_color *= stroke_points[i].flow;

                vec4 out_color;
                out_color.rgb = dst_color.rgb * (1.0 - src_color.a) + src_color.rgb;
                out_color.a = dst_color.a * (1.0 - src_color.a) + src_color.a;

                imageStore(tile_tex, tex_coord, mix(dst_color, out_color, mask));
            }
        }
    }
}