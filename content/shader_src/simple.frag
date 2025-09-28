#version 450

layout(set = 0, binding = 0) uniform sampler2D texture_sampler;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec3 fragNorm;
layout(location = 2) in vec3 fragPos;

layout(location = 0) out vec4 outColor;

void main() {
    const vec3 LIGHT_DIR = vec3(0.2, -1.0, 0.8);

    // Normalize interpolated normal
    vec3 N = normalize(fragNorm);
    vec3 L = normalize(-LIGHT_DIR); // light direction toward surface
    vec3 V = normalize(-fragPos);
    vec3 R = reflect(-L, N);

    // Phong components
    float ambient = 0.1;
    float diff = max(dot(N, L), 0.0);
    float spec = pow(max(dot(R, V), 0.0), 16.0); // 16 = shininess factor

    // Combine
    vec3 color = vec3(0.8, 0.8, 0.9) * (ambient + diff) + spec * vec3(1.0);
    outColor = vec4(color, 1.0);
}
