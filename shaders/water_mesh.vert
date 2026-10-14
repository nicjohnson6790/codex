#version 450
#extension GL_GOOGLE_include_directive : require
#include "atmosphere.glsl"
#include "water_displacement.glsl"

layout(set=0, binding=0) uniform sampler2DArray displacementTexture;

layout(set=0, binding=1, std430) readonly buffer HeightmapBuffer
{
    float heights[];
} heightmapBuffer;

#include "water_mesh_descriptors.glsl"


layout(location = 0) out vec3 fragWorldPosition;
layout(location = 1) flat out uint fragBandMask;
layout(location = 2) out float fragShoreFactor;
layout(location = 3) out float fragLocalDepth;
layout(location = 4) flat out uint fragHasTerrainSlice;
layout(location = 5) out float fragViewDistance;
layout(location = 6) out vec2 fragWaveDisplacementXZ;

#define WATER_UNIFORM_SET 1
#define WATER_UNIFORM_BINDING 0
#include "water_surface.glsl"

vec2 cornerCoord(uint selector)
{
    uint position = selector < 4u ? selector : selector + 1u;
    return vec2(position % 3u, position / 3u) * 0.5;
}

// Keep displaced depth/shore evaluation identical for interior and endpoints.
void endpoint(WaterInstance instance, vec2 coord, out vec3 position, out vec3 displacement,
    out float viewDistance, out float localDepth, out float shoreFactor)
{
    vec2 meters = coord * instance.leafParams.x;
    evaluateWaterSurface(instance, meters, position, displacement, viewDistance, localDepth, shoreFactor);
    if (instance.leafParams.w > 0.5)
    {
        float terrainHeight = sampleTerrainHeight(uint(max(instance.leafParams.z, 0.0)), meters, instance.leafParams.x);
        localDepth = max(water.cameraAndTime.z + position.y - terrainHeight, 0.0);
        shoreFactor = 1.0 - smoothstep(0.0, max(water.depthEffectParams.z, 0.001), localDepth);
    }
}

void boundaryEndpoint(WaterBoundaryEdge edge, uint edgeIndex, float along,
    out vec3 position, out vec3 displacement, out float viewDistance,
    out float localDepth, out float shoreFactor)
{
    bool coarse = (edge.mapping.x & 0x80000000u) != 0u;
    WaterInstance owner = edge.outer;
    float ownerAlong = coarse ? (along + float((edge.mapping.x >> 26u) & 1u)) * 0.5 : along;
    float side = coarse ? (edgeIndex < 2u ? 1.0 : 0.0) : (edgeIndex < 2u ? 0.0 : 1.0);
    vec2 coord = edgeIndex == 0u || edgeIndex == 2u ? vec2(side, ownerAlong) : vec2(ownerAlong, side);
    if (along == 0.0 || along == 1.0)
    {
        bool first = (edgeIndex == 0u || edgeIndex == 3u) ? along == 0.0 : along == 1.0;
        owner = first ? edge.firstCorner : edge.secondCorner;
        coord = cornerCoord(first ? edge.mapping.x & 7u : (edge.mapping.x >> 3u) & 7u);
    }
    endpoint(owner, coord, position, displacement, viewDistance, localDepth, shoreFactor);
}

void main()
{
    // Match the original 129x129 normalized grid without vertex attribute fetches.
    uint vertexIndex = uint(gl_VertexIndex);
    vec2 inLocalCoord = vec2(vertexIndex % 129u, vertexIndex / 129u) / 128.0;
    WaterInstance instance = descriptors.parents[gl_InstanceIndex].body;
    bool hasTerrainSlice = instance.leafParams.w > 0.5;
    uint bandMask = (instance.packedMetadata >> 16u) & 0xFFFFu;
    vec3 position, displacement;
    float viewDistance, localDepth, shoreFactor;
    if (inLocalCoord.x == 0.0 || inLocalCoord.y == 0.0 || inLocalCoord.x == 1.0 || inLocalCoord.y == 1.0)
    {
        uint edgeIndex = inLocalCoord.x == 0.0 ? 0u : inLocalCoord.y == 0.0 ? 1u : inLocalCoord.x == 1.0 ? 2u : 3u;
        WaterBoundaryEdge edge = descriptors.parents[gl_InstanceIndex].edges[edgeIndex];
        float along = edgeIndex == 0u || edgeIndex == 2u ? inLocalCoord.y : inLocalCoord.x;
        float intervals = float(edge.mapping.y);
        int coordinate = int(round(along * intervals));
        bool midpoint = (edge.mapping.x & 0x80000000u) != 0u && (coordinate & 1) != 0;
        boundaryEndpoint(edge, edgeIndex, midpoint ? float(coordinate - 1) / intervals : along,
            position, displacement, viewDistance, localDepth, shoreFactor);
        if (midpoint)
        {
            vec3 nextPosition, nextDisplacement;
            float nextDistance, nextDepth, nextShore;
            boundaryEndpoint(edge, edgeIndex, float(coordinate + 1) / intervals,
                nextPosition, nextDisplacement, nextDistance, nextDepth, nextShore);
            position = (position + nextPosition) * 0.5;
            displacement = (displacement + nextDisplacement) * 0.5;
            viewDistance = (viewDistance + nextDistance) * 0.5;
            localDepth = (localDepth + nextDepth) * 0.5;
            shoreFactor = (shoreFactor + nextShore) * 0.5;
        }
    }
    else
        endpoint(instance, inLocalCoord, position, displacement, viewDistance, localDepth, shoreFactor);
    fragWaveDisplacementXZ = displacement.xz;

    fragWorldPosition = position;
    fragBandMask = bandMask;
    fragShoreFactor = shoreFactor;
    fragLocalDepth = localDepth;
    fragHasTerrainSlice = hasTerrainSlice ? 1u : 0u;
    fragViewDistance = viewDistance;
    gl_Position = water.viewProjection * vec4(position, 1.0);
}
