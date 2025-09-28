#version 450

layout(push_constant) uniform PushConstants {
    mat4 world_transform;
	mat4 projection;
} pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNorm;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUV;

layout(location = 0) out vec2 fragUV; // for looking up textures
layout(location = 1) out vec3 fragPosTangentSpace; // finding view vector
layout(location = 2) out vec3 fragViewPosTangentSpace; // finding view vector
layout(location = 3) out vec3 fragLightDirTangentSpace; // directional light

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

const vec3 tangents[6] = vec3[](
    vec3( 1.0,  0.0,  0.0), // Front  (Aligned with +X)
    vec3(-1.0,  0.0,  0.0), // Back   (Aligned with -X)
    vec3( 0.0,  0.0,  1.0), // Left   (Aligned with +Z)
    vec3( 0.0,  0.0, -1.0), // Right  (Aligned with -Z)
    vec3( 1.0,  0.0,  0.0), // Top    (Aligned with +X)
    vec3( 1.0,  0.0,  0.0)  // Bottom (Aligned with +X)
);

// UV coordinates for each index
const vec2 uvs[36] = vec2[](
    vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0), // Front
    vec2(0.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0), // Back
    vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 0.0), vec2(1.0, 1.0), // Left
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 0.0), // Right
    vec2(0.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 0.0), // Top
    vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(1.0, 0.0), vec2(0.0, 1.0)  // Bottom
);

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
	fragLightDirTangentSpace = worldToTangentSpace * normalize(vec3(0.1,0.1,-1.0)); // directional light

	gl_Position.y *= -1.0;
}
