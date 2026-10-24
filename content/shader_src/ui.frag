#version 450

// Draws the game's UI (see ui.vert). The texture is either one that the UI draws with (a font's glyphs, an image), or the colors of
// a gradient, from ramp_start to ramp_end along it. Textures hold sRGB colors with the alpha premultiplied, as RmlUi makes them.

layout(set = 0, binding = 0) uniform sampler2D ui_texture;

layout(push_constant) uniform UIPushConstants {
    mat4 transform;
    vec2 translation;
    uint flags; // bit 0: multiply by the texture, bit 1: multiply by the gradient
    uint gradient_function; // 0: linear, 1: radial, 2: conic. 3, 4 and 5: the same, repeating
    vec2 gradient_p;        // linear: the starting point. Radial and conic: the centre
    vec2 gradient_v;        // linear: from the starting point to the end. Radial: 1 / the radius. Conic: the angle's unit vector
    float first_stop;       // where a repeating gradient repeats between
    float last_stop;
    float ramp_start;       // where the texture's first and last texels are along the gradient
    float ramp_end;
} constants;

layout(location = 0) in vec4 in_color; // linear, alpha premultiplied
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

const uint FLAG_TEXTURE = 1u;
const uint FLAG_GRADIENT = 2u;
const float PI = 3.14159265;

vec3 srgbToLinear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec4 decode(vec4 texel)
{
    const vec3 straight = (texel.a > 0.0) ? texel.rgb / texel.a : vec3(0.0);
    return vec4(srgbToLinear(straight) * texel.a, texel.a);
}

// How far along the gradient this pixel is (the same as RmlUi's OpenGL 3 renderer)
float gradientPosition()
{
    const uint function = constants.gradient_function % 3u;
    const vec2 p = constants.gradient_p;
    const vec2 v = constants.gradient_v;
    float t = 0.0;
    if (function == 0u) {
        t = dot(v, in_uv - p) / dot(v, v);
    }
    else if (function == 1u) {
        t = length(v * (in_uv - p));
    }
    else {
        const mat2 rotation = mat2(v.x, -v.y, v.y, v.x);
        const vec2 d = rotation * (in_uv - p);
        t = 0.5 + atan(-d.x, d.y) / (2.0 * PI);
    }
    if (constants.gradient_function >= 3u) {
        const float range = constants.last_stop - constants.first_stop;
        if (range > 0.0) {
            t = constants.first_stop + mod(t - constants.first_stop, range);
        }
    }
    return t;
}

void main()
{
    vec4 color = in_color;
    if ((constants.flags & FLAG_GRADIENT) != 0u) {
        const float ramp_length = max(constants.ramp_end - constants.ramp_start, 1e-6);
        const float u = clamp((gradientPosition() - constants.ramp_start) / ramp_length, 0.0, 1.0);
        // (from the centre of the first texel to the centre of the last)
        const float texels = float(textureSize(ui_texture, 0).x);
        color *= decode(textureLod(ui_texture, vec2((u * (texels - 1.0) + 0.5) / texels, 0.5), 0.0));
    }
    else if ((constants.flags & FLAG_TEXTURE) != 0u) {
        color *= decode(texture(ui_texture, in_uv));
    }
    out_color = color;
}
