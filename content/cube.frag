#version 450

layout(location = 0) in vec3 frag_normal;
layout(location = 1) in vec3 frag_world_pos;
layout(location = 2) in vec2 frag_uv;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D color_texture;

const vec3 light_dir = normalize(vec3(10.0, 5.0, -3.0));

void main() {
    float diffuse = 0.3 * dot(frag_normal, light_dir);
    float ambient = 0.1;

    vec3 view_dir = normalize(-frag_world_pos);
    vec3 reflect_dir = reflect(-light_dir, frag_normal);
    float spec = pow(max(dot(view_dir, reflect_dir), 0.0), 128);
    float specular = 1.0 * spec;  

    out_color = vec4(texture(color_texture, frag_uv).rgb * (diffuse + ambient + specular), 1.0);
}