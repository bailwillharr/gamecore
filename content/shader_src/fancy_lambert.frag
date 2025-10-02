#version 450

#define PI 3.1415926535897932384626433832795
#define PI_INV 0.31830988618379067153776752674503

layout(set = 0, binding = 0) uniform sampler2D materialSetBaseColorSampler;
layout(set = 0, binding = 1) uniform sampler2D materialSetORMSampler;
layout(set = 0, binding = 2) uniform sampler2D materialSetNormalSampler;

layout(location = 0) in vec2 fragUV; // for looking up textures
layout(location = 1) in vec3 fragPosTangentSpace; // finding view vector
layout(location = 2) in vec3 fragViewPosTangentSpace; // finding view vector
layout(location = 3) in vec3 fragLightPosTangentSpace; // point light

layout(location = 0) out vec4 outColor;

float GGXDist(float alpha_2, float N_dot_H) {
	const float num = alpha_2 * max(N_dot_H, 0.0);
	const float den = PI * pow(N_dot_H * N_dot_H * (alpha_2 - 1) + 1, 2.0);
	return num / den;
}

void main() {

	const vec3 albedo = vec3(texture(materialSetBaseColorSampler, fragUV));
	const vec3 orm = vec3(texture(materialSetORMSampler, fragUV));
	const float ao = orm.r;
	const float roughness = orm.g;
	const float metallic = orm.b;
	const vec3 N = normalize(vec3(texture(materialSetNormalSampler, fragUV)) * 2.0 - 1.0);
	//const vec3 N = vec3(0.0, 0.0, 1.0);

	const vec3 light_colour = vec3(1.0, 1.0, 1.0);
	const float light_intensity = 2.2 * 100.0;

	const float roughness_2 = roughness * roughness;

	const vec3 light_vec = fragLightPosTangentSpace - fragPosTangentSpace;
	const float light_distance = length(light_vec);
	const float attenuation = 1.0 / (0.5 + 0.2 * light_distance + 0.05 * light_distance * light_distance);

	const vec3 V = normalize(fragViewPosTangentSpace - fragPosTangentSpace);
	const vec3 L = normalize(light_vec);
	const vec3 H = normalize(V + L);

	const float L_dot_N = max(dot(L, N), 0.000001);
	const float L_dot_H = max(dot(L, H), 0.000001);
	const float V_dot_H = max(dot(V, H), 0.000001);
	const float V_dot_N = max(dot(V, N), 0.000001);
	const float N_dot_H = max(dot(N, H), 0.000001);

    vec3 diffuse_brdf = albedo * PI_INV;

	const float vis = ( max(L_dot_H, 0.0) / ( L_dot_N + sqrt(roughness_2 + (1 - roughness_2) * L_dot_N * L_dot_N) ) ) *
	( max(V_dot_H, 0.0) / ( V_dot_N + sqrt(roughness_2 + (1 - roughness_2) * V_dot_N * V_dot_N) ) );

	const vec3 specular_brdf = vec3(vis * GGXDist(roughness_2, N_dot_H));

	const vec3 dielectric_brdf = mix(diffuse_brdf, specular_brdf, 0.04 + (1 - 0.04) * pow(1 - abs(V_dot_H), 5));

	const vec3 metal_brdf = specular_brdf * (albedo + (1 - albedo) * pow(1 - V_dot_H, 5.0) );
	
	const vec3 brdf = mix(dielectric_brdf, metal_brdf, metallic);
	
	vec3 lighting = brdf * light_colour * attenuation * light_intensity * L_dot_N;

	//const vec3 ambient_light = vec3(0.1, 0.1, 0.1) * 2.4 * 0.3;
	//lighting += (ambient_light * ao * diffuse_brdf);

	// tone mapping
	const vec3 hdr_color = lighting;
	outColor = vec4(hdr_color / (hdr_color + 1.0), 1.0);
}
