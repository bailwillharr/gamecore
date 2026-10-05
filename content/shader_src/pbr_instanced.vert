#version 450

layout(set = 0, binding = 0) uniform FrameUniformBuffer {
    mat4 projection;
    mat4 view;
    vec3 camera_position;
} frame_uniform_buffer;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;

layout(location = 4) in vec4 model_matrix_col0;
layout(location = 5) in vec4 model_matrix_col1;
layout(location = 6) in vec4 model_matrix_col2;
layout(location = 7) in vec4 model_matrix_col3;

// in world space
layout(location = 0) out Vertex {
    vec3 position;
    vec3 normal;
    vec4 tangent; // w is the handedness of the bitangent
    vec2 texcoord;
} vout;

void main() {
    mat4 world_transform = mat4(model_matrix_col0, model_matrix_col1, model_matrix_col2, model_matrix_col3);

    vec4 world_position = world_transform * vec4(in_position, 1.0);

    mat3 normal_matrix = mat3(world_transform);

    vout.position = vec3(world_position);
    vout.normal = normalize(normal_matrix * in_normal);
    // Not normalised, as only materials with a normal map use it and the fragment shader has to normalise it anyway
    vout.tangent = vec4(normal_matrix * in_tangent.xyz, in_tangent.w);
    vout.texcoord = in_uv;

    gl_Position = frame_uniform_buffer.projection * frame_uniform_buffer.view * world_position;
}
