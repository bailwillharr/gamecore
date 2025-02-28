#version 450

layout(push_constant) uniform PushConstants {
    mat4 world_transform;
	mat4 projection;
} pc; 

layout(location = 0) out vec3 frag_normal;
layout(location = 1) out vec3 frag_world_pos;
layout(location = 2) out vec2 frag_uv;

// Cube vertex positions
const vec3 vertices[8] = vec3[](
    vec3(-1.0, -1.0, -1.0),
    vec3(-1.0, -1.0,  1.0),
    vec3(-1.0,  1.0, -1.0),
    vec3(-1.0,  1.0,  1.0),
    vec3( 1.0, -1.0, -1.0),
    vec3( 1.0, -1.0,  1.0),
    vec3( 1.0,  1.0, -1.0),
    vec3( 1.0,  1.0,  1.0)
);

// Index buffer
const uint indices[36] = uint[](
    2, 4, 0, 4, 2, 6, // Front
    3, 1, 5, 3, 5, 7, // Back
    0, 1, 2, 3, 2, 1, // Left
    6, 5, 4, 5, 6, 7, // Right
    2, 3, 6, 7, 6, 3, // Top
    4, 1, 0, 5, 1, 4 // Bottom
);

const vec3 normals[6] = vec3[](
    vec3( 0.0,  0.0, -1.0),
    vec3( 0.0,  0.0,  1.0),
    vec3(-1.0,  0.0,  0.0),
    vec3( 1.0,  0.0,  0.0),
    vec3( 0.0,  1.0,  0.0),
    vec3( 0.0, -1.0,  0.0)
);

void main() {
    frag_normal = normalize(mat3(transpose(inverse(pc.world_transform))) * normals[gl_VertexIndex / 6]);
    frag_world_pos = vec3(pc.world_transform * vec4(vertices[indices[gl_VertexIndex]], 1.0));
    frag_uv = (vertices[indices[gl_VertexIndex]].xy + vec2(1.0, 1.0)) * 0.5;
    gl_Position = pc.projection * pc.world_transform * vec4(vertices[indices[gl_VertexIndex]], 1.0f);
    gl_Position.y *= -1.0;
}
