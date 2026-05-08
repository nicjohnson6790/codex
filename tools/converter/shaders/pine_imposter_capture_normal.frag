#version 450

layout(set=3, binding=0) uniform ImposterCaptureMaterialUniforms
{
    vec4 viewBasisRight;
    vec4 viewBasisUp;
    vec4 viewBasisForward;
    vec4 sunDirectionIntensity;
    vec4 sunColorAmbient;
    vec4 shadingParams0;
    vec4 cameraPositionAlphaCutoff;
} capture;

layout(set=2, binding=0) uniform sampler2D baseColorTexture;
layout(set=2, binding=1) uniform sampler2D normalTexture;
layout(set=2, binding=2) uniform sampler2D roughnessTexture;
layout(set=2, binding=3) uniform sampler2D specularTexture;
layout(set=2, binding=4) uniform sampler2D aoTexture;
layout(set=2, binding=5) uniform sampler2D subsurfaceTexture;

layout(location = 0) in vec2 fragUv0;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragTangent;
layout(location = 3) in vec3 fragBitangent;
layout(location = 4) in vec3 fragWorldPosition;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 albedoSample = texture(baseColorTexture, fragUv0);
    if (albedoSample.a < capture.cameraPositionAlphaCutoff.w)
    {
        discard;
    }

    vec3 tangentNormal = texture(normalTexture, fragUv0).xyz * 2.0 - 1.0;
    if (!gl_FrontFacing)
    {
        tangentNormal.xy *= -1.0;
    }

    mat3 tbn = mat3(
        normalize(fragTangent),
        normalize(fragBitangent),
        normalize(fragNormal));
    vec3 normal = normalize(tbn * tangentNormal);
    if (!gl_FrontFacing)
    {
        normal = -normal;
    }

    vec3 viewNormal = normalize(vec3(
        dot(normal, capture.viewBasisRight.xyz),
        dot(normal, capture.viewBasisUp.xyz),
        dot(normal, capture.viewBasisForward.xyz)));

    outColor = vec4((viewNormal * 0.5) + 0.5, albedoSample.a);
}
