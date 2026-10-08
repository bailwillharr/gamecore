#version 450

// Draws the sky (see SkyboxComponent): one triangle that covers the screen, at the far plane. The pipeline only lets it through
// where nothing else has been drawn. Drawn with three vertices and no vertex buffers.

// (the same block as pbr.frag's, up to what is needed here)
layout(set = 0, binding = 0) uniform FrameUniformBuffer {
    mat4 projection;
    mat4 view;
    vec3 camera_position;
    uint point_light_count;
    vec4 directional_light_direction;
    vec4 directional_light_color;
    float exposure;                   // scales luminance (cd/m^2) to the range that is tone mapped
    vec4 ambient_light;
    mat4 shadow_matrix;
    vec4 shadow_params;
    mat4 environment_matrix;          // turns a direction in the world to one in the sky's cube maps
    vec4 environment_params;          // z = the luminance (cd/m^2) of a texel of 1
} frame_uniform_buffer;

// The direction that is seen at this point of the screen, in the space of the sky's cube map. Not normalised
layout(location = 0) out vec3 out_direction;

void main() {
    // (-1, -1), (3, -1), (-1, 3): the screen is the part of this triangle from -1 to 1
    vec2 position = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;

    // Undo the projection to get the direction from the camera (which looks down -Z), then turn it to the world and to the sky.
    // The sky is infinitely far away, so where the camera is doesn't come into it.
    vec3 view_direction = vec3(position.x / frame_uniform_buffer.projection[0][0], position.y / frame_uniform_buffer.projection[1][1], -1.0);
    vec3 world_direction = transpose(mat3(frame_uniform_buffer.view)) * view_direction;
    out_direction = mat3(frame_uniform_buffer.environment_matrix) * world_direction;

    // The depth buffer is reversed: zero is the far plane
    gl_Position = vec4(position, 0.0, 1.0);
}
