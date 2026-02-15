#version 450

layout(set = 0, binding = 0) uniform sampler2D materialSetBaseColorSampler;
layout(set = 0, binding = 1) uniform sampler2D materialSetORMSampler;
layout(set = 0, binding = 2) uniform sampler2D materialSetNormalSampler;

layout(location = 0) in Vertex {
    vec3 position;
    vec2 texcoord;
    vec3 light_position;
    vec3 tangent;
    vec3 bitangent;
    vec3 normal;
} vin;

layout(location = 0) out vec4 color;

void main()
{
	// Sample input textures to get shading model params.
	vec3 albedo = texture(materialSetBaseColorSampler, vin.texcoord).rgb;
    vec3 orm = texture(materialSetORMSampler, vin.texcoord).rgb;
	float metalness = orm.z;
	float roughness = orm.y;

    vec3 tangent_normal = texture(materialSetNormalSampler, vin.texcoord).xyz;
    tangent_normal = normalize(tangent_normal * 2.0 - 1.0);

    mat3 TBN = mat3(
        normalize(vin.tangent),
        normalize(vin.bitangent),
        normalize(vin.normal)
    );

    vec3 L = normalize(vin.light_position - vin.position);

    tangent_normal = vec3(0.0, 0.0, 1.0);

    vec3 N = normalize(TBN * tangent_normal);

    //N = normalize(vin.normal);

    vec3 direct_lighting = max(dot(L, N), 0.0) * vec3(1.0);

	// Final fragment color.
	color = vec4(direct_lighting, 1.0);
}
