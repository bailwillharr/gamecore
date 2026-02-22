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
    vec2 texcoord;
} vout;

void main() {
    vout.texcoord = vec2(in_uv.x, 1.0 - in_uv.y);
    gl_Position = pc.projection * pc.view * pc.world_transform * vec4(in_position, 1.0);
}
