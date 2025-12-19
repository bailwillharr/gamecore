#version 450

// No vertex buffer used here.
// Use vkCmdDraw(cmd, 36, 1, 0, 0)

layout(push_constant) uniform PushConstants {
    mat4 world_transform;
	mat4 view;
	mat4 projection;
	vec3 light_pos;
} pc;

layout(location = 0) out vec3 frag_direction;

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

const uint indices[36] = uint[](
    2, 4, 0, 4, 2, 6,
    3, 1, 5, 3, 5, 7,
    0, 1, 2, 3, 2, 1,
    6, 5, 4, 5, 6, 7,
    2, 3, 6, 7, 6, 3,
    4, 1, 0, 5, 1, 4
);

void main() {
	vec3 frag_position = vertices[indices[gl_VertexIndex]];
    
    frag_direction = frag_position;

    vec4 pos = vec4(mat3(pc.view) * frag_position, 1.0);
    gl_Position = pc.projection * pos;
    gl_Position.z = 0.0;
}
