#version 450

layout(push_constant) uniform PushConstants {
    mat4 world_transform;
	mat4 projection;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNorm;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUV;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec3 fragNorm;
layout(location = 2) out vec3 fragPos;

void main() {
	fragUV = inUV;

	fragNorm = mat3(transpose(inverse(pc.world_transform))) * inNorm;

	vec4 worldPosition = pc.world_transform * vec4(inPosition, 1.0);
	fragPos = vec3(worldPosition);
	gl_Position = pc.projection * worldPosition;
	gl_Position.y *= -1.0;
}
