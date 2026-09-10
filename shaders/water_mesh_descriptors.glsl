struct WaterInstance
{
    vec3 position;
    uint packedMetadata;
    vec4 leafParams;
};
struct WaterParent
{
    WaterInstance body;
    uvec4 edges;
};
// Capacity and stride match QuadtreeWaterMeshRenderer::Descriptors.
layout(set=0, binding=2, std430) readonly buffer DescriptorBuffer
{
    WaterParent parents[4096];
    uint bridges[];
} descriptors;
