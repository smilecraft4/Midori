#version 460

// Optimize for tile only
layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;


layout(set = 2, binding = 0) uniform Stroke {
    vec4 stroke_color;
    float stroke_flow;
    float stroke_radius;
    float stroke_hardness;
    uint stroke_points_num;
};

layout(set = 2, binding = 1) uniform Tile {
    vec2 tile_pos;
    vec2 tile_size;
};

struct StrokePointData {
    vec2 position;
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
        if(d <= stroke_radius) {
            imageStore(tile_tex, tex_coord, stroke_color);
        }
    }
}