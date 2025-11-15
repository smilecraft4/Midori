#version 460

// Optimize for tile only
layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;

layout(set = 2, binding = 0) uniform Merge {
};

layout(set = 1, binding = 1, rgba8) uniform image2D src_tex;
layout(set = 1, binding = 1, rgba8) uniform image2D dst_tex;

void main() {
    // Get pixel position
    // Apply simple premultiplied alpha blending
}