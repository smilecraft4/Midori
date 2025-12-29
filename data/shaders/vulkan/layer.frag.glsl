#version 460

layout(location = 0) out vec4 out_color;

layout(location = 0) in vec2 in_uv;
layout(location = 1) flat in float in_opacity;
layout(location = 2) flat in uint in_blend_mode;

layout(set = 2, binding = 0) uniform sampler2D layer_sampler;
// layout(set = 2, binding = 1) uniform sampler2D swapchain_sampler;

vec3 LinearToSRGB(vec3 rgb) {
  // See https://gamedev.stackexchange.com/questions/92015/optimized-linear-to-srgb-glsl
  return mix(1.055 * pow(rgb, vec3(1.0 / 2.4)) - 0.055,
             rgb * 12.92,
             lessThanEqual(rgb, vec3(0.0031308)));
}


void main() {
    vec4 layer_color = texture(layer_sampler, in_uv);
    layer_color.rgb *= layer_color.a;
    // layer_color.rgb = LinearToSRGB(layer_color.rgb);
    
    out_color = layer_color;
}