#version 460

layout(location = 0) out vec2 out_uv;
layout(location = 1) out float out_opacity;
layout(location = 2) out uint out_blend_mode;

layout(set = 1, binding = 0) uniform Layer {
    float opacity;
    uint blend_mode;
};

const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

const vec2 uvs[3] = vec2[](
    vec2(0.0, 0.0),
    vec2(2.0, 0.0),
    vec2(0.0, 2.0)
);

void main() {
    vec2 pos = positions[gl_VertexIndex];
    
    out_uv = uvs[gl_VertexIndex];
    out_opacity = opacity;
    out_blend_mode = blend_mode;

    gl_Position = vec4(pos, 0.0, 1.0);
}