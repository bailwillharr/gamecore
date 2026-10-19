#version 450

// Shades a terrain (see terrain.vert). Lit like everything else (pbr.frag, which this copies the lighting of), except that:
//  - The terrain's material is tiled across the ground: its textures are a few metres across, seen at two sizes and turned
//    against each other so that the repeats don't line up. Its base colour constant tints the textures.
//  - The ground gets darker and rockier where it is steep.
//  - There are no baked shadows: a terrain is too big for one shadow map.

const float PI = 3.141592;
const float Epsilon = 0.00001;
const vec3 Fdielectric = vec3(0.04);

// How big the material's textures are on the ground, in metres. The second is turned by 37 degrees
const float DETAIL_SIZE = 3.0;
const float DETAIL_SIZE_LARGE = 23.0;
const mat2 DETAIL_TURN = mat2(0.7986, 0.6018, -0.6018, 0.7986);
// The base colour is also varied over this distance, so that the ground isn't the same colour everywhere far away
const float VARIATION_SIZE = 701.0;
// Beyond this distance the normal map fades out, as its bumps are smaller than a pixel
const float DETAIL_NORMAL_FADE = 250.0;

layout(constant_id = 0) const bool USE_BASE_COLOR_TEXTURE = true;
layout(constant_id = 1) const bool USE_ORM_TEXTURE = true;
layout(constant_id = 2) const bool USE_NORMAL_TEXTURE = true;
layout(constant_id = 3) const bool USE_EMISSIVE_TEXTURE = true;

const int MAX_POINT_LIGHTS = 32; // same as WorldDrawData::MAX_POINT_LIGHTS

struct PointLight {
    vec4 position_range;
    vec4 color;
};

// (see pbr.frag)
layout(set = 0, binding = 0) uniform FrameUniformBuffer {
    mat4 projection;
    mat4 view;
    vec3 camera_position;
    uint point_light_count;
    vec4 directional_light_direction;
    vec4 directional_light_color;
    float exposure;
    vec4 ambient_light;
    mat4 shadow_matrix;
    vec4 shadow_params;
    mat4 environment_matrix;
    vec4 environment_params;
    vec4 fog_params;
    vec4 fog_color;
    PointLight point_lights[MAX_POINT_LIGHTS];
} frame_uniform_buffer;

layout(push_constant) uniform PushConstants {
    layout(offset = 64) vec4 base_color;
    layout(offset = 80) float roughness;
    layout(offset = 84) float metallic;
    layout(offset = 88) float alpha_cutoff;
    layout(offset = 96) vec3 emissive;
} material;

layout(set = 1, binding = 0) uniform sampler2D materialSetBaseColorSampler;
layout(set = 1, binding = 1) uniform sampler2D materialSetORMSampler;
layout(set = 1, binding = 2) uniform sampler2D materialSetNormalSampler;

layout(set = 3, binding = 1) uniform samplerCube environmentIrradianceSampler;
layout(set = 3, binding = 2) uniform samplerCube environmentSpecularSampler;

layout(location = 0) in Vertex {
    vec3 position;
    vec3 normal;
    vec2 terrain_position;
} vin;
layout(location = 3) flat in uint in_level;

layout(location = 0) out vec4 color;

float ndfGGX(float cosLh, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSq = alpha * alpha;
    float denom = (cosLh * cosLh) * (alphaSq - 1.0) + 1.0;
    return alphaSq / (PI * denom * denom);
}

float gaSchlickG1(float cosTheta, float k) { return cosTheta / (cosTheta * (1.0 - k) + k); }

float gaSchlickGGX(float cosLi, float cosLo, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return gaSchlickG1(cosLi, k) * gaSchlickG1(cosLo, k);
}

vec3 fresnelSchlick(vec3 F0, float cosTheta) { return F0 + (vec3(1.0) - F0) * pow(1.0 - cosTheta, 5.0); }

vec2 environmentBRDF(float roughness, float cosLo)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * cosLo)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

vec3 Uncharted2Tonemap(vec3 x)
{
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 shadeLight(vec3 Li, vec3 Lradiance, vec3 N, vec3 Lo, float cosLo, vec3 F0, vec3 albedo, float roughness, float metalness)
{
    vec3 Lh = normalize(Li + Lo);
    float cosLi = max(0.0, dot(N, Li));
    float cosLh = max(0.0, dot(N, Lh));
    vec3 F = fresnelSchlick(F0, max(0.0, dot(Lh, Lo)));
    float D = ndfGGX(cosLh, roughness);
    float G = gaSchlickGGX(cosLi, cosLo, roughness);
    vec3 kd = mix(vec3(1.0) - F, vec3(0.0), metalness);
    vec3 diffuseBRDF = kd * albedo / PI;
    vec3 specularBRDF = (F * D * G) / max(Epsilon, 4.0 * cosLi * cosLo);
    return (diffuseBRDF + specularBRDF) * Lradiance * cosLi;
}

// Fades a luminance into the fog (see FogComponent) between the camera and 'position'. Returns how much of it is still seen.
// The fog is density * exp(-falloff * (height - base)) per metre, which can be added up along the line exactly.
float applyFog(inout vec3 luminance, vec3 position)
{
    float density = frame_uniform_buffer.fog_params.x;
    if (density <= 0.0) {
        return 1.0;
    }
    float falloff = frame_uniform_buffer.fog_params.y;
    vec3 to_point = position - frame_uniform_buffer.camera_position;
    float distance = length(to_point);
    float density_at_camera = density * exp(-falloff * (frame_uniform_buffer.camera_position.z - frame_uniform_buffer.fog_params.z));
    float rise = falloff * to_point.z;
    float optical_depth = density_at_camera * distance * ((abs(rise) > 1.0e-4) ? (1.0 - exp(-rise)) / rise : 1.0);
    float transmittance = exp(-optical_depth);

    vec3 fog_luminance = frame_uniform_buffer.fog_color.rgb;
    if (frame_uniform_buffer.fog_params.w > 0.0) {
        // The light that the haze scatters towards the camera is about what the sky looks like that way, near the horizon
        vec3 direction = to_point / max(distance, 1.0e-3);
        direction.z = max(direction.z, 0.03);
        vec3 sky_direction = mat3(frame_uniform_buffer.environment_matrix) * normalize(direction);
        fog_luminance = textureLod(environmentSpecularSampler, sky_direction, 0.6 * frame_uniform_buffer.environment_params.y).rgb *
                        frame_uniform_buffer.fog_params.w;
    }
    luminance = mix(fog_luminance, luminance, transmittance);
    return transmittance;
}

// Smooth noise from 0 to 1, which changes over about 1 unit
float valueNoise(vec2 p)
{
    vec2 cell = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    // (a hash of the cell's corners)
    vec4 corners = vec4(cell.x, cell.x + 1.0, cell.x, cell.x + 1.0) * 127.1 + vec4(cell.y, cell.y, cell.y + 1.0, cell.y + 1.0) * 311.7;
    vec4 h = fract(sin(mod(corners, 6283.1853)) * 43758.5453);
    return mix(mix(h.x, h.y, f.x), mix(h.z, h.w, f.x), f.y);
}

// Colours for the levels of detail, for debugging
const vec3 LEVEL_COLORS[8] = vec3[8](vec3(0.8, 0.1, 0.1), vec3(0.8, 0.5, 0.1), vec3(0.7, 0.8, 0.1), vec3(0.1, 0.8, 0.2), vec3(0.1, 0.7, 0.8),
                                     vec3(0.1, 0.2, 0.8), vec3(0.5, 0.1, 0.8), vec3(0.8, 0.1, 0.6));

void main()
{
    vec3 Lo_unnormalised = frame_uniform_buffer.camera_position - vin.position;
    float view_distance = length(Lo_unnormalised);
    vec3 Lo = Lo_unnormalised / max(view_distance, 1.0e-4);
    vec3 geometry_normal = normalize(vin.normal);

    vec2 uv_small = vin.terrain_position / DETAIL_SIZE;
    vec2 uv_large = (DETAIL_TURN * vin.terrain_position) / DETAIL_SIZE_LARGE;

    vec3 albedo = material.base_color.rgb;
    if (USE_BASE_COLOR_TEXTURE) {
        vec3 small = texture(materialSetBaseColorSampler, uv_small).rgb;
        vec3 large = texture(materialSetBaseColorSampler, uv_large).rgb;
        albedo *= mix(small, large, 0.5);
    }
    albedo *= mix(0.8, 1.2, valueNoise(vin.terrain_position / VARIATION_SIZE));

    // Steep ground is bare rock: darker and greyer than the dust that settles on flatter ground
    float steepness = smoothstep(0.85, 0.6, geometry_normal.z);
    albedo = mix(albedo, albedo * vec3(0.55, 0.5, 0.5), steepness);

    float metalness = material.metallic;
    float roughness = material.roughness;
    float occlusion = 1.0;
    if (USE_ORM_TEXTURE) {
        vec3 orm = texture(materialSetORMSampler, uv_small).rgb;
        metalness = orm.z;
        roughness = orm.y;
        occlusion = orm.x;
    }

    vec3 N = geometry_normal;
    if (USE_NORMAL_TEXTURE) {
        // The textures lie flat along x and y, so their tangent is along x, tilted with the ground
        vec3 T = normalize(vec3(N.z, 0.0, -N.x));
        vec3 B = cross(N, T);
        vec3 small = 2.0 * texture(materialSetNormalSampler, uv_small).rgb - 1.0;
        vec3 large = 2.0 * texture(materialSetNormalSampler, uv_large).rgb - 1.0;
        large.xy = transpose(DETAIL_TURN) * large.xy; // (the large texture is turned, so its bumps are too)
        vec3 tangent_space_normal = normalize(vec3(small.xy + large.xy, small.z * large.z));
        float strength = 1.0 - smoothstep(0.5 * DETAIL_NORMAL_FADE, DETAIL_NORMAL_FADE, view_distance);
        tangent_space_normal.xy *= strength;
        N = normalize(mat3(T, B, N) * normalize(tangent_space_normal));
    }

    if ((in_level & 16u) != 0u) {
        albedo = LEVEL_COLORS[in_level & 7u] * 0.5;
    }

    float cosLo = max(0.0, dot(N, Lo));
    vec3 F0 = mix(Fdielectric, albedo, metalness);

    vec3 sun_direction = frame_uniform_buffer.directional_light_direction.xyz;
    vec3 direct = shadeLight(sun_direction, frame_uniform_buffer.directional_light_color.rgb, N, Lo, cosLo, F0, albedo, roughness, metalness);
    // Bumps in the normal map can't make the ground face the sun if the ground itself is facing away from it
    direct *= smoothstep(-0.05, 0.05, dot(geometry_normal, sun_direction));

    for (uint i = 0; i < frame_uniform_buffer.point_light_count; ++i) {
        vec3 to_light = frame_uniform_buffer.point_lights[i].position_range.xyz - vin.position;
        float range = frame_uniform_buffer.point_lights[i].position_range.w;
        float distance_squared = max(dot(to_light, to_light), 0.0001);
        float attenuation = 1.0 / distance_squared;
        if (range > 0.0) {
            float window = distance_squared / (range * range);
            window = clamp(1.0 - window * window, 0.0, 1.0);
            attenuation *= window * window;
        }
        if (attenuation > 0.0) {
            vec3 Li = to_light * inversesqrt(distance_squared);
            direct += shadeLight(Li, frame_uniform_buffer.point_lights[i].color.rgb * attenuation, N, Lo, cosLo, F0, albedo, roughness, metalness);
        }
    }

    vec3 ambient;
    if (frame_uniform_buffer.environment_params.x > 0.0) {
        mat3 world_to_sky = mat3(frame_uniform_buffer.environment_matrix);
        vec3 F = F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - cosLo, 5.0);
        vec3 kd = mix(vec3(1.0) - F, vec3(0.0), metalness);
        vec3 diffuse = kd * albedo * texture(environmentIrradianceSampler, world_to_sky * N).rgb;
        vec3 reflected = world_to_sky * reflect(-Lo, N);
        vec3 reflection = textureLod(environmentSpecularSampler, reflected, roughness * frame_uniform_buffer.environment_params.y).rgb;
        vec2 brdf = environmentBRDF(roughness, cosLo);
        vec3 specular = reflection * (F0 * brdf.x + brdf.y);
        ambient = (diffuse + specular) * frame_uniform_buffer.environment_params.x;
    }
    else {
        vec3 sky_illuminance = frame_uniform_buffer.ambient_light.rgb;
        vec3 ground_illuminance = frame_uniform_buffer.ambient_light.rgb * 0.5;
        ambient = mix(ground_illuminance, sky_illuminance, 0.5 * (N.z + 1.0)) * albedo / PI;
    }
    ambient *= occlusion;

    vec3 luminance = direct + ambient;
    applyFog(luminance, vin.position);

    const float ExposureBias = 2.0;
    const float WhitePoint = 11.2;
    vec3 exposed = frame_uniform_buffer.exposure * luminance;
    vec3 mapped = Uncharted2Tonemap(ExposureBias * exposed) / Uncharted2Tonemap(vec3(WhitePoint)).x;
    color = vec4(mapped, 1.0);
}
