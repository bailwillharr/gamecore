#version 450

layout(set = 0, binding = 0) uniform samplerCube cube_sampler;

layout(location = 0) in vec3 frag_direction;

layout(location = 0) out vec4 out_color;

void main() {
	out_color = texture(cube_sampler, frag_direction);
}