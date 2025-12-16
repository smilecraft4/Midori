#version 460

/// Merge is always src onto dst
// for now the default premultiplied alpha is used
// This only work for the same number of pixels so:
// assert((src_size.x * src_size.y) == (dst_size.x * dst_size.y))
// the src_start and dst_start can be different but the
// region must be contained within the texture so: 
// assert((src_start.x + src_size.x) <= src_tex.x)
// assert((src_start.y + src_size.y <= src_tex.y)
// assert((dst_start.x + dst_size.x) <= dst_tex.x)
// assert(dst_start.y + dst_size.y <= dst_tex.y)

// I guess for now the layout size will be optimized for tile size
// but this code could also be used for blending layers before 
// rendering by using a canvas texture the size of the viewport
// the layer texture should be the same size but in case

// For blending the dst_blend_mode is maybe not really important
// Planned blend_mode are:
// Normal (premultiplied alpha)
// Add
// Multiply
// Overlay

// All layers texture are treated as premultiplied alpha Linear 
// All color inputs are treated as startight alpha SRGB

layout(set = 2, binding = 0) uniform Merge {
    uint src_blend_mode;
    float src_opacity;
    ivec2 src_start;
    ivec2 src_size;
    float dst_opacity;
    uint dst_blend_mode;
    ivec2 dst_pos;
    ivec2 dst_size;
};

layout(set = 0, binding = 0) uniform sampler2D src_tex;
layout(set = 1, binding = 0, rgba8) uniform image2D dst_tex;

layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;
void main() {
    const ivec2 coord = ivec2(gl_GlobalInvocationID.xy);

    vec4 dstColor = imageLoad(dst_tex, coord);
    vec4 srcColor = texelFetch(src_tex, coord, 0);
    srcColor *= src_opacity;

    dstColor.rgb = srcColor.rgb + dstColor.rgb * (1.0 - srcColor.a);
    dstColor.a = srcColor.a + dstColor.a * (1.0 - srcColor.a);

    imageStore(dst_tex, coord, dstColor);
}