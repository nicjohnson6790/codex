struct WaterInstance
{
    vec3 position;
    uint packedMetadata;
    vec4 leafParams;
};
struct WaterBoundaryEdge
{
    WaterInstance outer;
    WaterInstance firstCorner;
    WaterInstance secondCorner;
    uvec4 mapping;
};
struct WaterParent
{
    WaterInstance body;
    WaterBoundaryEdge edges[4];
};
// Capacity and stride match QuadtreeWaterMeshRenderer::Descriptors.
layout(set=0, binding=2, std430) readonly buffer DescriptorBuffer
{
    WaterParent parents[4096];
} descriptors;
