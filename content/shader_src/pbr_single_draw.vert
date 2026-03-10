#version 450

layout(push_constant) uniform PushConstants {
    mat4 world_transform;
} pc;

layout(set = 0, binding = 0) uniform FrameUniformBuffer {
    mat4 projection;
    mat4 view;
    vec3 camera_position;
} frame_uniform_buffer;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;

layout(location = 0) out Vertex {
    vec3 position;
    vec3 eye_position;
    vec3 light_direction;
    vec2 texcoord;
} vout;

void main() {
    vec4 world_position = pc.world_transform * vec4(in_position, 1.0);

    mat3 normal_matrix = mat3(pc.world_transform);

    vec3 N = normalize(normal_matrix * in_normal);
    vec3 T = normalize(normal_matrix * in_tangent.xyz);
    T = normalize(T - dot(T, N) * N); // re-orthogonalise tangent
    vec3 B = cross(N, T) * in_tangent.w;

    mat3 world_to_tangent_space = transpose(mat3(T, B, N));

    vout.position = world_to_tangent_space * vec3(world_position);
    vout.eye_position = world_to_tangent_space * frame_uniform_buffer.camera_position;
    vout.light_direction = world_to_tangent_space * vec3(1.0, 1.0, 1.0);
    vout.texcoord = in_uv;

    gl_Position = frame_uniform_buffer.projection * frame_uniform_buffer.view * world_position;
}
