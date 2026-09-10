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

layout(location = 0) in vec2 inLocalCoord;

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

void main()
{
    WaterInstance instance = descriptors.parents[gl_InstanceIndex].body;
    float leafSize = instance.leafParams.x;
    float waterLevel = instance.leafParams.y;
    uint terrainSliceIndex = uint(max(instance.leafParams.z, 0.0));
    bool hasTerrainSlice = instance.leafParams.w > 0.5;
    uint bandMask = (instance.packedMetadata >> 16u) & 0xFFFFu;
    uint cascadeCount = uint(max(water.waterParams.w, 0.0));

    vec2 localMeters = inLocalCoord * leafSize;
    vec3 position, displacement;
    float viewDistance, localDepth, shoreFactor;
    evaluateWaterSurface(instance, localMeters, position, displacement,
        viewDistance, localDepth, shoreFactor);
    fragWaveDisplacementXZ = displacement.xz;

    if (hasTerrainSlice)
    {
        float terrainHeight = sampleTerrainHeight(terrainSliceIndex, localMeters, leafSize);
        float displacedWaterHeight = water.cameraAndTime.z + position.y;
        localDepth = max(displacedWaterHeight - terrainHeight, 0.0);

        float shoreDepth = max(water.depthEffectParams.z, 0.001);
        shoreFactor = 1.0 - smoothstep(0.0, shoreDepth, localDepth);
    }

    fragWorldPosition = position;
    fragBandMask = bandMask;
    fragShoreFactor = shoreFactor;
    fragLocalDepth = localDepth;
    fragHasTerrainSlice = hasTerrainSlice ? 1u : 0u;
    fragViewDistance = viewDistance;
    gl_Position = water.viewProjection * vec4(position, 1.0);
}
