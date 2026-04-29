#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragWorldPosition;
layout(location = 2) in float fragLodBlend;

layout(location = 0) out vec4 outColor;

void main()
{
    vec3 normal = normalize(fragNormal);
    vec3 viewDir = normalize(vec3(0.0, 1.0, 0.2));
    float fresnel = pow(1.0 - max(dot(normal, viewDir), 0.0), 5.0);

    vec3 deepColor = vec3(0.03, 0.18, 0.30);
    vec3 shallowColor = vec3(0.08, 0.34, 0.52);
    vec3 lodTint = mix(vec3(0.10, 0.20, 0.28), vec3(0.03, 0.12, 0.18), fragLodBlend);
    vec3 color = mix(shallowColor, deepColor, fresnel) + (lodTint * 0.25);

    outColor = vec4(color, 1.0);
}
