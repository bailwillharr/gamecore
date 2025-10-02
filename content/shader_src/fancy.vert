#version 450

layout(push_constant) uniform PushConstants {
    mat4 world_transform;
	mat4 projection;
	vec3 light_pos;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNorm;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUV;

layout(location = 0) out vec2 fragUV; // for looking up textures
layout(location = 1) out vec3 fragPosTangentSpace; // finding view vector
layout(location = 2) out vec3 fragViewPosTangentSpace; // finding view vector
layout(location = 3) out vec3 fragLightPosTangentSpace; // point light

void main() {
	vec4 worldPosition = pc.world_transform * vec4(inPosition, 1.0);
	gl_Position = pc.projection * worldPosition;
	
	mat3 normal_matrix = transpose(inverse(mat3(pc.world_transform)));

	vec3 T = normalize(normal_matrix * inTangent.xyz);
	vec3 N = normalize(normal_matrix * inNorm);
	vec3 B = cross(T, N) * inTangent.w;
	mat3 worldToTangentSpace = transpose(mat3(T, B, N));
	
	fragUV = inUV;
	fragPosTangentSpace = worldToTangentSpace * vec3(worldPosition);
	fragViewPosTangentSpace = worldToTangentSpace * vec3(0.0, 0.0, 0.0);
	fragLightPosTangentSpace = worldToTangentSpace * pc.light_pos;

	gl_Position.y *= -1.0;
}
