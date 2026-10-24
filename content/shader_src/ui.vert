#version 450

// Draws the game's UI (RmlUi's triangles, see gc_ui.h and gc_render_ui.cpp), over the world.

layout(location = 0) in vec2 in_position; // in pixels, from the top left
layout(location = 1) in vec4 in_color;    // sRGB, alpha premultiplied
layout(location = 2) in vec2 in_uv;

// Must match UIPushConstants in gc_render_ui.cpp
layout(push_constant) uniform UIPushConstants {
    mat4 transform; // from pixels to clip space
    vec2 translation;
    uint flags;
    uint gradient_function;
    vec2 gradient_p;
    vec2 gradient_v;
    float first_stop;
    float last_stop;
    float ramp_start;
    float ramp_end;
} constants;

layout(location = 0) out vec4 out_color; // linear, alpha premultiplied
layout(location = 1) out vec2 out_uv;

vec3 srgbToLinear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main()
{
    gl_Position = constants.transform * vec4(in_position + constants.translation, 0.0, 1.0);

    // The framebuffer is sRGB, so the color is made linear here (the alpha has to be taken out to do that)
    const vec3 straight = (in_color.a > 0.0) ? in_color.rgb / in_color.a : vec3(0.0);
    out_color = vec4(srgbToLinear(straight) * in_color.a, in_color.a);
    out_uv = in_uv;
}
