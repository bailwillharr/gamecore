#version 450

layout(set = 0, binding = 0) uniform sampler2D materialSetBaseColorSampler;
layout(set = 0, binding = 1) uniform sampler2D materialSetORMSampler;
layout(set = 0, binding = 2) uniform sampler2D materialSetNormalSampler;

layout(location = 0) in Vertex {
    vec2 texcoord;
} vin;

layout(location = 0) out vec4 color;

vec3 linearToSRGB(vec3 c)
{
    vec3 s = vec3(0.0);
    for (int i = 0; i < 3; ++i)
    {
        if (c[i] <= 0.0031308)
            s[i] = 12.92 * c[i];
        else
            s[i] = 1.055 * pow(c[i], 1.0/2.4) - 0.055;
    }
    return s;
}

void main()
{
	// Sample input textures to get shading model params.
	vec3 albedo = texture(materialSetBaseColorSampler, vin.texcoord).rgb;
    //albedo = linearToSRGB(albedo);

	// Final fragment color.
	color = vec4(albedo, 1.0);
}
