#version 450

layout(set=1, binding=0) uniform ImposterCaptureUniforms
{
    mat4 viewProjection;
} imposter;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUv0;

layout(location = 0) out vec2 fragUv0;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragTangent;
layout(location = 3) out vec3 fragBitangent;
layout(location = 4) out vec3 fragWorldPosition;

void main()
{
    vec3 tangent = normalize(inTangent.xyz);
    vec3 normal = normalize(inNormal);
    vec3 bitangent = normalize(cross(normal, tangent)) * inTangent.w;

    gl_Position = imposter.viewProjection * vec4(inPosition, 1.0);
    fragUv0 = vec2(inUv0.x, 1.0 - inUv0.y);
    fragNormal = normal;
    fragTangent = tangent;
    fragBitangent = bitangent;
    fragWorldPosition = inPosition;
}
