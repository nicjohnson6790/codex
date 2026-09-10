struct TerrainInstance
{
    vec3 position;
    uint packedMetadata;
};
struct TerrainParent
{
    TerrainInstance body;
    uvec4 edges[4];
};
// Capacity and stride match QuadtreeMeshRenderer::Descriptors.
layout(set=0, binding=1, std430) readonly buffer DescriptorBuffer
{
    TerrainParent parents[512];
    uint bridges[];
} descriptors;
