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
layout(location = 4) in vec3 fragLightDirTangentSpace; // directional light

layout(location = 0) out vec4 outColor;

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    // Schlick approximation
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

float distributionGGX(vec3 N, vec3 H, float roughness)
{
    float a      = roughness*roughness;
    float a2     = a*a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;

    float num   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = 3.141592 * denom * denom;

    return num / denom;
}

float geometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r*r) / 8.0;

    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggxV = geometrySchlickGGX(NdotV, roughness);
    float ggxL = geometrySchlickGGX(NdotL, roughness);
    return ggxV * ggxL;
}

vec3 BRDF_PBR(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness)
{
    vec3 H = normalize(V + L);
    
    // Fresnel
    vec3 F0 = mix(vec3(0.04), albedo, metallic); // F0 = 0.04 for dielectrics, albedo for metals
    vec3 F  = fresnelSchlick(max(dot(H, V), 0.0), F0);

    // Distribution & Geometry
    float D = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);

    // Specular
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    vec3 spec = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);

    // Diffuse
    vec3 kD = (1.0 - F) * (1.0 - metallic);  // remove diffuse for metals

    float LdotH = max(dot(L, H), 0.0);
    float R_R = 2.0 * roughness * LdotH * LdotH;
	float one_minus_L_dot_N = 1.0 - NdotL;
	float one_minus_V_dot_N = 1.0 - NdotV;
	float F_L = one_minus_L_dot_N * one_minus_L_dot_N * one_minus_L_dot_N * one_minus_L_dot_N * one_minus_L_dot_N;
	float F_V = one_minus_V_dot_N * one_minus_V_dot_N * one_minus_V_dot_N * one_minus_V_dot_N * one_minus_V_dot_N;
	vec3 f_retro_reflection = albedo * PI_INV * R_R * (F_L + F_V + F_L*F_V*(R_R - 1.0) );
	vec3 diffuse_brdf = albedo * PI_INV * (1.0 - 0.5 * F_L) * (1.0 - 0.5 * F_V) + f_retro_reflection;
    vec3 diffuse = kD * diffuse_brdf;

    // Final
    return (diffuse + spec) * NdotL;
}

void main() {

	const vec3 BASE_COLOR = vec3(texture(materialSetBaseColorSampler, fragUV));
	const vec3 orm = vec3(texture(materialSetORMSampler, fragUV));
	const float AO = orm.r;
	const float ROUGHNESS = orm.g;
	const float METALLIC = orm.b;
	
	const vec3 N = normalize(vec3(texture(materialSetNormalSampler, fragUV)) * 2.0 - 1.0);
	const vec3 V = normalize(fragViewPosTangentSpace - fragPosTangentSpace);

	const vec3 light_colour = normalize(vec3(1.0, 0.8, 0.6));
	const float light_intensity = 2.2 * 25.0;
	const vec3 light_vec = fragLightPosTangentSpace - fragPosTangentSpace;
	const float light_distance = length(light_vec);
	const float attenuation = 1.0 / (0.5 + 0.2 * light_distance + 0.05 * light_distance * light_distance);
	const vec3 point_light = light_colour * light_intensity * attenuation;

	vec3 L = normalize(light_vec);
	//vec3 hdr_color = BRDF_PBR(N, V, L, BASE_COLOR, METALLIC, ROUGHNESS);
    
    //vec3 hdr_color = vec3(1.0, 1.0, 1.0) * fragPosTangentSpace;

    //L = normalize(fragLightDirTangentSpace);
    //hdr_color += BRDF_PBR(N, V, L, BASE_COLOR, METALLIC, ROUGHNESS) * vec3(1.0, 0.9, 1.0) * 2.2;
    
	//outColor = vec4(hdr_color / (hdr_color + 1.0), 1.0);

	outColor = vec4(fragPosTangentSpace, 1.0);
}
