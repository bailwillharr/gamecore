#version 450

layout(push_constant) uniform PushConstants {
    mat4 world_transform;
} pc;

// Cube vertex positions
const vec3 vertices[8] = vec3[](
    vec3(-1.0, -1.0, -1.0),
    vec3( 1.0, -1.0, -1.0),
    vec3( 1.0,  1.0, -1.0),
    vec3(-1.0,  1.0, -1.0),
    vec3(-1.0, -1.0,  1.0),
    vec3( 1.0, -1.0,  1.0),
    vec3( 1.0,  1.0,  1.0),
    vec3(-1.0,  1.0,  1.0)
);

// Index buffer
const uint indices[36] = uint[](
    0, 1, 2, 2, 3, 0,  // Front
    4, 5, 6, 6, 7, 4,  // Back
    0, 4, 7, 7, 3, 0,  // Left
    1, 5, 6, 6, 2, 1,  // Right
    3, 2, 6, 6, 7, 3,  // Top
    0, 1, 5, 5, 4, 0   // Bottom
);

void main() {
    gl_Position.xyz = vertices[indices[gl_VertexIndex]];
    gl_Position.w = 1.0;
    gl_Position.y *= -1.0;
}