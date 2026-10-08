
#version 450

// Physically Based Rendering
// Copyright (c) 2017-2018 Michał Siejak

// Physically Based shading model: Lambetrtian diffuse BRDF + Cook-Torrance microfacet specular BRDF + IBL for ambient.

// This implementation is based on "Real Shading in Unreal Engine 4" SIGGRAPH 2013 course notes by Epic Games.
// See: http://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf

const float PI = 3.141592;
const float Epsilon = 0.00001;

// Constant normal incidence Fresnel factor for all dielectrics.
const vec3 Fdielectric = vec3(0.04);

// Which textures the material has. Each combination is drawn with its own pipeline (see RenderBackend::setWorldShaders()),
// so a material without, say, a normal map doesn't pay for sampling one.
layout(constant_id = 0) const bool USE_BASE_COLOR_TEXTURE = true;
layout(constant_id = 1) const bool USE_ORM_TEXTURE = true;
layout(constant_id = 2) const bool USE_NORMAL_TEXTURE = true;
layout(constant_id = 3) const bool USE_EMISSIVE_TEXTURE = true;

// What the material's alpha does (MaterialBlendMode). This also has a pipeline for each value.
const int BLEND_MODE_NONE = 0;        // opaque
const int BLEND_MODE_ALPHA_TEST = 1;  // nothing is drawn where the alpha is below the material's cutoff
const int BLEND_MODE_ALPHA_BLEND = 2; // the alpha is written, and the pipeline blends with it
layout(constant_id = 4) const int BLEND_MODE = BLEND_MODE_NONE;

const int MAX_POINT_LIGHTS = 32; // same as WorldDrawData::MAX_POINT_LIGHTS

struct PointLight {
    vec4 position_range; // xyz = world position, w = range (zero or less: unlimited)
    vec4 color;          // rgb = color * luminous intensity (candela)
};

layout(set = 0, binding = 0) uniform FrameUniformBuffer {
    mat4 projection;
    mat4 view;
    vec3 camera_position;
    uint point_light_count;
    vec4 directional_light_direction; // xyz = direction towards the light
    vec4 directional_light_color;     // rgb = color * illuminance (lux). Black if the world has no directional light
    float exposure;                   // scales luminance (cd/m^2) to the range that is tone mapped
    vec4 ambient_light;               // rgb = color * illuminance (lux) of a surface facing up. Black if the world has no ambient light.
                                      // Not used if the world is lit by a sky
    mat4 shadow_matrix;               // world space to the shadow map's texture coordinates (xy) and depth (z)
    vec4 shadow_params;               // x = normal bias (metres), y = depth bias, z = 1 if there is a shadow map, else 0
    mat4 environment_matrix;          // turns a direction in the world to one in the sky's cube maps
    vec4 environment_params;          // x = the luminance (cd/m^2) of a texel of 1, or 0 if the world isn't lit by a sky.
                                      // y = the last mip level of the sky's reflections
    PointLight point_lights[MAX_POINT_LIGHTS];
} frame_uniform_buffer;

// What is used in place of the textures that the material doesn't have. Bytes 0-63 are the vertex shader's.
layout(push_constant) uniform PushConstants {
    layout(offset = 64) vec4 base_color; // linear
    layout(offset = 80) float roughness;
    layout(offset = 84) float metallic;
    layout(offset = 88) float alpha_cutoff;
    layout(offset = 96) vec3 emissive; // the emissive texture is multiplied by this. Black if the material doesn't glow
} material;

layout(set = 1, binding = 0) uniform sampler2D materialSetBaseColorSampler;
layout(set = 1, binding = 1) uniform sampler2D materialSetORMSampler;
layout(set = 1, binding = 2) uniform sampler2D materialSetNormalSampler;
layout(set = 1, binding = 3) uniform sampler2D materialSetEmissiveSampler;

// The depth of the surface nearest to the directional light, baked when the world was made (see ShadowMapComponent).
// 0 is nearest to the light and 1 is farthest.
layout(set = 2, binding = 0) uniform sampler2D shadowMapSampler;

// The sky (see SkyboxComponent), in cube maps that were worked out from it when it was packaged. Black if the world has none.
// The irradiance is the light that a surface facing each direction gets from the whole sky, divided by pi: the luminance of a
// white diffuse surface. The reflections are the sky as a surface reflects it: sharp in the first mip level, for a mirror, and
// blurrier in each level after, up to a roughness of 1 in the last.
layout(set = 3, binding = 1) uniform samplerCube environmentIrradianceSampler;
layout(set = 3, binding = 2) uniform samplerCube environmentSpecularSampler;

// in world space
layout(location = 0) in Vertex {
    vec3 position;
    vec3 normal;
    vec4 tangent; // w is the handedness of the bitangent
    vec2 texcoord;
} vin;

layout(location = 0) out vec4 color;

// GGX/Towbridge-Reitz normal distribution function.
// Uses Disney's reparametrization of alpha = roughness^2.
float ndfGGX(float cosLh, float roughness)
{
	float alpha   = roughness * roughness;
	float alphaSq = alpha * alpha;

	float denom = (cosLh * cosLh) * (alphaSq - 1.0) + 1.0;
	return alphaSq / (PI * denom * denom);
}

// Single term for separable Schlick-GGX below.
float gaSchlickG1(float cosTheta, float k)
{
	return cosTheta / (cosTheta * (1.0 - k) + k);
}

// Schlick-GGX approximation of geometric attenuation function using Smith's method.
float gaSchlickGGX(float cosLi, float cosLo, float roughness)
{
	float r = roughness + 1.0;
	float k = (r * r) / 8.0; // Epic suggests using this roughness remapping for analytic lights.
	return gaSchlickG1(cosLi, k) * gaSchlickG1(cosLo, k);
}

// Shlick's approximation of the Fresnel factor.
vec3 fresnelSchlick(vec3 F0, float cosTheta)
{
	return F0 + (vec3(1.0) - F0) * pow(1.0 - cosTheta, 5.0);
}

// How much of the light from all around is reflected, for a surface of some roughness seen at some angle: the reflectance is
// F0 * x + y. This is the second half of the "split sum" in the course notes above (the first half is the sky's reflections
// cube map). It is usually looked up in a texture. This is a curve fitted to that texture, from Karis, "Physically Based
// Shading on Mobile" (2014).
vec2 environmentBRDF(float roughness, float cosLo)
{
	const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
	const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
	vec4 r = roughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * cosLo)) * r.x + r.y;
	return vec2(-1.04, 1.04) * a004 + r.zw;
}

// Cubic approximation of the planckian (black body) locus. This is a very good approximation for most purposes.
// Returns chromaticity vec2 (x/y, no luminance) in xyY space.
// Technically only designed for 1667K < T < 25000K, but you can push it further.

// Credit to B. Kang et al. (2002) (https://api.semanticscholar.org/CorpusID:4489377)
// Note: there may be a patent associated with this function
// TODO: if()s are not shader-friendly. find faster method.
vec2 PLANCKIAN_LOCUS_CUBIC_XY(float T) {
    vec2 xy = vec2(0.0, 0.0);
    if(T < 4000.0) {
        xy.x = -0.2661239*1000000000.0/(T*T*T) - 0.2343589*1000000.0/(T*T) + 0.8776956*1000.0/T + 0.179910;

        if(T < 2222.0) xy.y = -1.1063814*xy.x*xy.x*xy.x - 1.34811020*xy.x*xy.x + 2.18555832*xy.x - 0.20219683; 
        else           xy.y = -0.9549476*xy.x*xy.x*xy.x - 1.37418593*xy.x*xy.x + 2.09137015*xy.x -  0.16748867;
    } else {
        xy.x = -3.0258469*1000000000.0/(T*T*T) + 2.1070379*1000000.0/(T*T) + 0.2226347*1000.0/T + 0.24039;

        xy.y = 3.08175806*xy.x*xy.x*xy.x - 5.8733867*xy.x*xy.x + 3.75112997*xy.x - 0.37001483;
    }
    return xy;
}

vec3 XYY_TO_XYZ(vec3 xyY) {
    return vec3(
        xyY.z * xyY.x / xyY.y,
        xyY.z,
        xyY.z * (1.0 - xyY.x - xyY.y) / xyY.y
    );
}

vec3 Uncharted2Tonemap(vec3 x)
{
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;
    return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F)) - E/F;
}

// How much of the directional light reaches a surface: 0 if it is in shadow, 1 if it is lit.
// N is the surface's normal (not the normal map's) and Li is the direction towards the light.
float directionalLightVisibility(vec3 position, vec3 N, vec3 Li)
{
	if (frame_uniform_buffer.shadow_params.z == 0.0) {
		return 1.0;
	}

	// A surface would shadow itself, as a texel of the shadow map covers an area of it at a single depth. Look up a point a little
	// way off the surface instead, further for surfaces that the light only grazes.
	float cosLi = clamp(dot(N, Li), 0.0, 1.0);
	vec3 offset_position = position + N * (frame_uniform_buffer.shadow_params.x * sqrt(1.0 - cosLi * cosLi));

	vec3 shadow_coord = (frame_uniform_buffer.shadow_matrix * vec4(offset_position, 1.0)).xyz;
	if (any(lessThan(shadow_coord, vec3(0.0))) || any(greaterThan(shadow_coord, vec3(1.0)))) {
		return 1.0; // outside what was baked
	}
	float depth = shadow_coord.z - frame_uniform_buffer.shadow_params.y;

	// Percentage closer filtering: compare with the 4x4 texels around the point, weighted so that the result is a 3x3 texel box
	// filter that moves smoothly with the point.
	ivec2 size = textureSize(shadowMapSampler, 0);
	vec2 texel = shadow_coord.xy * vec2(size) - 0.5;
	ivec2 base = ivec2(floor(texel));
	vec2 f = fract(texel);
	vec4 weights_x = vec4(1.0 - f.x, 1.0, 1.0, f.x);
	vec4 weights_y = vec4(1.0 - f.y, 1.0, 1.0, f.y);
	float lit = 0.0;
	for (int y = 0; y < 4; ++y) {
		for (int x = 0; x < 4; ++x) {
			ivec2 coord = clamp(base + ivec2(x - 1, y - 1), ivec2(0), size - 1);
			float nearest_depth = texelFetch(shadowMapSampler, coord, 0).r;
			lit += weights_x[x] * weights_y[y] * ((depth <= nearest_depth) ? 1.0 : 0.0);
		}
	}
	return lit / 9.0;
}

// What one light adds. Li is the direction towards the light and Lradiance is the illuminance (lux) of a surface facing it.
vec3 shadeLight(vec3 Li, vec3 Lradiance, vec3 N, vec3 Lo, float cosLo, vec3 F0, vec3 albedo, float roughness, float metalness)
{
	// Half-vector between Li and Lo.
	vec3 Lh = normalize(Li + Lo);

	// Calculate angles between surface normal and various light vectors.
	float cosLi = max(0.0, dot(N, Li));
	float cosLh = max(0.0, dot(N, Lh));

	// Calculate Fresnel term for direct lighting.
	vec3 F  = fresnelSchlick(F0, max(0.0, dot(Lh, Lo)));
	// Calculate normal distribution for specular BRDF.
	float D = ndfGGX(cosLh, roughness);
	// Calculate geometric attenuation for specular BRDF.
	float G = gaSchlickGGX(cosLi, cosLo, roughness);

	// Diffuse scattering happens due to light being refracted multiple times by a dielectric medium.
	// Metals on the other hand either reflect or absorb energy, so diffuse contribution is always zero.
	// To be energy conserving we must scale diffuse BRDF contribution based on Fresnel factor & metalness.
	vec3 kd = mix(vec3(1.0) - F, vec3(0.0), metalness);

	// Lambert diffuse BRDF.
	// Scaled by 1/PI as lights are in physical units: illuminance (lux) in, luminance (cd/m^2) out.
	vec3 diffuseBRDF = kd * albedo / PI;

	// Cook-Torrance specular microfacet BRDF.
	vec3 specularBRDF = (F * D * G) / max(Epsilon, 4.0 * cosLi * cosLo);

	// Total contribution for this light.
	return (diffuseBRDF + specularBRDF) * Lradiance * cosLi;
}

void main()
{
	// Get shading model params from the material's textures, or from its constants if it doesn't have them.
	vec3 albedo = material.base_color.rgb;
	float alpha = material.base_color.a;
	if (USE_BASE_COLOR_TEXTURE) {
		vec4 base_color = texture(materialSetBaseColorSampler, vin.texcoord); // an sRGB texture, so the color is linear
		albedo = base_color.rgb;
		alpha = base_color.a;
	}
	if (BLEND_MODE == BLEND_MODE_ALPHA_TEST && alpha < material.alpha_cutoff) {
		discard;
	}
	float metalness = material.metallic;
	float roughness = material.roughness;
	float occlusion = 1.0; // how much of the light from all around gets into the surface's nooks: only dims the sky and ambient light
	if (USE_ORM_TEXTURE) {
		vec3 orm = texture(materialSetORMSampler, vin.texcoord).rgb;
		metalness = orm.z;
		roughness = orm.y;
		occlusion = orm.x;
	}

	// Outgoing light direction (vector from world-space fragment position to the "eye").
	vec3 Lo = normalize(frame_uniform_buffer.camera_position - vin.position);

	// Get current fragment's normal in world space.
	vec3 N = normalize(vin.normal);
	vec3 surface_normal = N;
	if (USE_NORMAL_TEXTURE) {
		vec3 T = normalize(vin.tangent.xyz - dot(vin.tangent.xyz, N) * N); // re-orthogonalise tangent
		vec3 B = cross(N, T) * vin.tangent.w;
		vec3 tangent_space_normal = 2.0 * texture(materialSetNormalSampler, vin.texcoord).rgb - 1.0;
		N = normalize(mat3(T, B, N) * tangent_space_normal);
	}

	// Angle between surface normal and outgoing light direction.
	float cosLo = max(0.0, dot(N, Lo));

	// Fresnel reflectance at normal incidence (for metals use albedo color).
	vec3 F0 = mix(Fdielectric, albedo, metalness);

	// directional light (its color is black if there isn't one)
	vec3 sun_direction = frame_uniform_buffer.directional_light_direction.xyz;
	vec3 directLighting = shadeLight(sun_direction, frame_uniform_buffer.directional_light_color.rgb, N, Lo, cosLo, F0, albedo, roughness, metalness);
	directLighting *= directionalLightVisibility(vin.position, surface_normal, sun_direction);

	// point lights
	for (uint i = 0; i < frame_uniform_buffer.point_light_count; ++i) {
		vec3 to_light = frame_uniform_buffer.point_lights[i].position_range.xyz - vin.position;
		float range = frame_uniform_buffer.point_lights[i].position_range.w;
		float distance_squared = max(dot(to_light, to_light), 0.0001);

		// Inverse square falloff: candela to lux.
		float attenuation = 1.0 / distance_squared;
		if (range > 0.0) {
			// Windowed so that it smoothly reaches zero at the light's range (from the UE4 course notes above).
			float window = distance_squared / (range * range);
			window = clamp(1.0 - window * window, 0.0, 1.0);
			attenuation *= window * window;
		}

		if (attenuation > 0.0) {
			vec3 Li = to_light * inversesqrt(distance_squared);
			directLighting += shadeLight(Li, frame_uniform_buffer.point_lights[i].color.rgb * attenuation, N, Lo, cosLo, F0, albedo, roughness, metalness);
		}
	}

    // The light from all around
    vec3 ambient;
    if (frame_uniform_buffer.environment_params.x > 0.0) {
        // From the sky (image based lighting). The sky's sun isn't in these cube maps: it is the directional light above.
        mat3 world_to_sky = mat3(frame_uniform_buffer.environment_matrix);

        // Diffuse: what the whole sky gives a surface facing this way. As for a light, less is scattered the more is reflected,
        // and metals scatter none.
        vec3 F = F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - cosLo, 5.0); // Fresnel, weaker for rough surfaces
        vec3 kd = mix(vec3(1.0) - F, vec3(0.0), metalness);
        vec3 diffuse = kd * albedo * texture(environmentIrradianceSampler, world_to_sky * N).rgb;

        // Specular: the part of the sky that is mirrored towards the eye, as blurred as the surface is rough
        vec3 reflected = world_to_sky * reflect(-Lo, N);
        vec3 reflection = textureLod(environmentSpecularSampler, reflected, roughness * frame_uniform_buffer.environment_params.y).rgb;
        vec2 brdf = environmentBRDF(roughness, cosLo);
        vec3 specular = reflection * (F0 * brdf.x + brdf.y);

        ambient = (diffuse + specular) * frame_uniform_buffer.environment_params.x;
    }
    else {
        // From the world's ambient lights (black if it has none). Surfaces facing up get all of it, surfaces facing down get half.
        vec3 skyIlluminance    = frame_uniform_buffer.ambient_light.rgb;
        vec3 groundIlluminance = frame_uniform_buffer.ambient_light.rgb * 0.5;
        float NdotUp = 0.5 * (N.z + 1.0); // remap [-1,1] → [0,1]. The world is Z-up
        ambient = mix(groundIlluminance, skyIlluminance, NdotUp) * albedo / PI;
    }
    ambient *= occlusion;

    // luminance, in cd/m^2
    vec3 total_lighting = directLighting + ambient;

    // The light that the material gives off itself. It isn't lit by anything and doesn't light anything: it is just added to what
    // is seen. It is added after the exposure, so that something that glows looks the same in a dark world as in a bright one:
    // an emission of 1 is about as bright as a white surface in full light.
    vec3 emissive = material.emissive;
    if (USE_EMISSIVE_TEXTURE) {
        emissive *= texture(materialSetEmissiveSampler, vin.texcoord).rgb; // an sRGB texture, so this is linear
    }

    // Tone map. The curve is divided by its value at the white point so that white maps to 1.
    const float ExposureBias = 2.0;
    const float WhitePoint = 11.2;
    vec3 exposed = frame_uniform_buffer.exposure * total_lighting + emissive;
    vec3 mapped = Uncharted2Tonemap(ExposureBias * exposed) / Uncharted2Tonemap(vec3(WhitePoint)).x;

	// Final fragment color. This is linear: the render target has an sRGB format, so it is gamma encoded when it is written.
	color = vec4(mapped, (BLEND_MODE == BLEND_MODE_ALPHA_BLEND) ? alpha : 1.0);
}
