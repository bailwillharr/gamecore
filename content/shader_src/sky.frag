#version 450

// Draws the sky (see sky.vert). The sky is a light source like any other, so it goes through the same exposure and tone mapping
// as everything it lights (see pbr.frag).

layout(set = 0, binding = 0) uniform FrameUniformBuffer {
    mat4 projection;
    mat4 view;
    vec3 camera_position;
    uint point_light_count;
    vec4 directional_light_direction;
    vec4 directional_light_color;
    float exposure;                   // scales luminance (cd/m^2) to the range that is tone mapped
    vec4 ambient_light;
    mat4 shadow_matrix;
    vec4 shadow_params;
    mat4 environment_matrix;          // turns a direction in the world to one in the sky's cube maps
    vec4 environment_params;          // z = the luminance (cd/m^2) of a texel of 1
} frame_uniform_buffer;

// the sky as it is seen
layout(set = 3, binding = 0) uniform samplerCube environmentSkySampler;

layout(location = 0) in vec3 in_direction;

layout(location = 0) out vec4 color;

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

void main()
{
    // luminance, in cd/m^2
    vec3 sky = texture(environmentSkySampler, in_direction).rgb * frame_uniform_buffer.environment_params.z;

    // the same tone mapping as pbr.frag
    const float ExposureBias = 2.0;
    const float WhitePoint = 11.2;
    vec3 exposed = frame_uniform_buffer.exposure * sky;
    vec3 mapped = Uncharted2Tonemap(ExposureBias * exposed) / Uncharted2Tonemap(vec3(WhitePoint)).x;

    color = vec4(mapped, 1.0);
}
