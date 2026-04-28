#version 450

layout(location = 0) in vec2 fragNdc;
layout(location = 0) out vec4 outColor;

layout(set=2, binding=0) uniform samplerCube skyboxTexture;

layout(set=3, binding=0) uniform SkyboxUniforms
{
    mat4 inverseViewProjection;
    mat4 skyRotation;
} uniforms;

void main()
{
    vec4 clipPosition = vec4(fragNdc, 0.0, 1.0);
    vec4 worldPosition = uniforms.inverseViewProjection * clipPosition;
    vec3 worldDirection = normalize(worldPosition.xyz / max(worldPosition.w, 0.00001));
    vec3 sampleDirection = transpose(mat3(uniforms.skyRotation)) * worldDirection;
    outColor = texture(skyboxTexture, sampleDirection);
}
