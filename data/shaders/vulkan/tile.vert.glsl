#version 460

layout(location = 0) out vec2 out_uv;

layout(set = 1, binding = 0) uniform Viewport {
    mat4 proj;
    mat4 view;
};

layout(set = 1, binding = 1) uniform Tile {
    vec2 position;
    vec2 size;
};

const vec2 vertices[4] = vec2[](
    vec2(0.0, 1.0),
    vec2(0.0, 0.0),
    vec2(1.0, 1.0),
    vec2(1.0, 0.0)
);

const vec2 uvs[4] = vec2[](
    vec2(0.0, 0.0),
    vec2(0.0, 1.0),
    vec2(1.0, 0.0),
    vec2(1.0, 1.0)
);

void main() {
    vec2 pos = (vertices[gl_VertexIndex] + position) * size;
    out_uv = uvs[gl_VertexIndex];

    gl_Position = proj * view * vec4(pos, 0.0, 1.0);
}
