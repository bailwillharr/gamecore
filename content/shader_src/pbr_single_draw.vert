#version 450

layout(push_constant) uniform PushConstants {
    mat4 world_transform;
	mat4 view;
	mat4 projection;
	vec3 light_pos;
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;

layout(location = 0) out Vertex {
    vec3 position;
    mat3 tangent_basis;
    vec2 texcoord;
    vec3 eye_position;
    vec3 light_position;
} vout;

void main() {
    mat3 normal_matrix = transpose(inverse(mat3(pc.world_transform)));

    vec3 N = normalize(normal_matrix * in_normal);
    vec3 T = normalize(normal_matrix * in_tangent.xyz);
    T = normalize(T - dot(T, N) * N); // re-orthogonalise tangent
    vec3 B = cross(N, T) * in_tangent.w;

    vout.position = vec3(pc.world_transform * vec4(in_position, 1.0));
    vout.tangent_basis = mat3(T, B, N);
    vout.texcoord = vec2(in_uv.x, 1.0 - in_uv.y);
    vout.eye_position = vec3(inverse(pc.view) * vec4(0.0, 0.0, 0.0, 1.0));
    vout.light_position = pc.light_pos;

    gl_Position = pc.projection * pc.view * pc.world_transform * vec4(in_position, 1.0);
}
