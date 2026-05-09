#version 450

layout(set=2, binding=0) uniform sampler2DArray baseColorTextureArray;

struct NearbyMaterialGpu
{
    uvec4 layers0;
    uvec4 layers1;
    vec4 params;
};

layout(set=2, binding=1, std430) readonly buffer NearbyMaterialBuffer
{
    NearbyMaterialGpu materials[];
} materialBuffer;

layout(location = 0) in vec2 fragUv0;
layout(location = 5) flat in uint fragMaterialIndex;

layout(location = 0) out vec4 outColor;

void main()
{
    NearbyMaterialGpu material = materialBuffer.materials[fragMaterialIndex];
    float alphaCutoff = material.params.x;
    float baseColorLayer = float(material.layers0.x);
    vec4 albedoSample = texture(baseColorTextureArray, vec3(fragUv0, baseColorLayer));
    if (albedoSample.a < alphaCutoff)
    {
        discard;
    }

    outColor = vec4(0.0);
}
