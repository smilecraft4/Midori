#version 460

layout(location = 0) out vec4 out_color;

layout(location = 0) in vec2 in_uv;

layout(set = 2, binding = 0) uniform sampler2D tile_sampler;

void main() {
    out_color = texture(tile_sampler, in_uv);
}
