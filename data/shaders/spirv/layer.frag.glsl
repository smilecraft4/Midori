#version 460

layout(location = 0) out vec4 out_color;

layout(location = 0) in vec2 in_uv;
layout(location = 1) flat in float in_opacity;
layout(location = 2) flat in uint in_blend_mode;

layout(set = 2, binding = 0) uniform sampler2D layer_sampler;

void main() {
    vec4 layer_color = texture(layer_sampler, in_uv);
    layer_color.rgb *= layer_color.a;
    
    out_color = layer_color;
}