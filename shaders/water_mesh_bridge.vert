#version 450

layout(set=1, binding=0) uniform WaterUniforms
{
    mat4 viewProjection;
    vec4 cameraAndTime;
    vec4 waterParams;
    vec4 sunDirectionIntensity;
    vec4 sunColorAmbient;
    vec4 debugParams;
    vec4 cascadeWorldSizesA;
    vec4 cascadeWorldSizesB;
    vec4 cascadeShallowDampingA;
    vec4 cascadeShallowDampingB;
    vec4 cascadeShallowDepthA;
    vec4 cascadeShallowDepthB;
    vec4 depthEffectParams;
    mat4 skyRotation;
    vec4 atmosphereParams;
    vec4 sunDirectionTimeOfDay;
    vec4 opticalParams;
    vec4 refractionParams;
    vec4 distanceLodParams;
    vec4 cascadeFilterParams;
    vec4 farFieldParams;
    vec4 foamLodParams;
    vec4 foamParams;
    vec4 foamParams2;
    vec4 foamColor;
    vec4 foamDetailShape;
    vec4 foamDetailRidges;
    vec4 foamDetailBreakup;
    vec4 foamEvolutionParams;
    vec4 foamFadeParams;
    vec4 shorelineFoamParams;
    vec4 shorelineFoamDecayParams;
    vec4 shallowWaterColor;
    vec4 midWaterColor;
    vec4 deepWaterColor;
    vec4 waterDepthColorParams;
} water;

layout(set=0, binding=0) uniform sampler2DArray displacementTexture;

layout(set=0, binding=1, std430) readonly buffer HeightmapBuffer
{
    float heights[];
} heightmapBuffer;

struct WaterInstance
{
    vec3 position;
    uint packedMetadata;
    vec4 leafParams;
};

layout(set=0, binding=2, std430) readonly buffer InstanceBuffer
{
    WaterInstance instanceData[];
} instanceBuffer;

layout(location = 0) in vec2 inLocalCoord;

layout(location = 0) out vec3 fragWorldPosition;
layout(location = 1) flat out uint fragBandMask;
layout(location = 2) out float fragShoreFactor;
layout(location = 3) out float fragLocalDepth;
layout(location = 4) flat out uint fragHasTerrainSlice;
layout(location = 5) out float fragViewDistance;

const uint kHeightmapResolution = 259u;
const uint kHeightmapMaxCoord = kHeightmapResolution - 1u;
const float kHeightmapLeafIntervalCount = 256.0;

float cascadeWorldSize(uint cascadeIndex)
{
    if (cascadeIndex < 4u)
    {
        return water.cascadeWorldSizesA[cascadeIndex];
    }

    return water.cascadeWorldSizesB[cascadeIndex - 4u];
}

float cascadeShallowDamping(uint cascadeIndex)
{
    if (cascadeIndex < 4u)
    {
        return water.cascadeShallowDampingA[cascadeIndex];
    }

    return water.cascadeShallowDampingB[cascadeIndex - 4u];
}

float cascadeShallowDepth(uint cascadeIndex)
{
    if (cascadeIndex < 4u)
    {
        return water.cascadeShallowDepthA[cascadeIndex];
    }

    return water.cascadeShallowDepthB[cascadeIndex - 4u];
}

float cascadeShallowFade(uint cascadeIndex, float localDepth)
{
    float fadeStart = max(cascadeShallowDepth(cascadeIndex), water.depthEffectParams.y + 0.001);
    float fadeEnd = max(water.depthEffectParams.y, 0.0);
    return smoothstep(fadeEnd, fadeStart, localDepth);
}

float metersPerPixel(float viewDistance)
{
    float viewportHeight = max(water.distanceLodParams.x, 1.0);
    float tanHalfVerticalFov = max(water.distanceLodParams.y, 1.0e-4);
    return max((2.0 * tanHalfVerticalFov * viewDistance) / viewportHeight, 1.0e-4);
}

float cascadeDetailWeight(float worldSize, float metersPerPixelAtView)
{
    float texelWorldSize = worldSize / 512.0;
    float resolvedTexelScale = texelWorldSize / metersPerPixelAtView;
    return smoothstep(
        water.cascadeFilterParams.x,
        water.cascadeFilterParams.y,
        resolvedTexelScale);
}

float sampleTerrainHeight(uint sliceIndex, vec2 localMeters, float leafSize)
{
    float sampleSpacing = leafSize / kHeightmapLeafIntervalCount;
    vec2 sampleCoord = vec2(1.0) + (localMeters / max(sampleSpacing, 1.0e-5));
    ivec2 clampedCoord = clamp(ivec2(round(sampleCoord)), ivec2(0), ivec2(int(kHeightmapMaxCoord)));
    uint linearIndex =
        (sliceIndex * kHeightmapResolution * kHeightmapResolution) +
        (uint(clampedCoord.y) * kHeightmapResolution) +
        uint(clampedCoord.x);
    return heightmapBuffer.heights[linearIndex];
}

vec2 rotateBridgeLocalCoord(uint edgeIndex, vec2 localCoord)
{
    if (edgeIndex == 0u)
    {
        return localCoord;
    }

    if (edgeIndex == 1u)
    {
        return vec2(1.0 - localCoord.y, localCoord.x);
    }

    if (edgeIndex == 2u)
    {
        return vec2(1.0 - localCoord.x, 1.0 - localCoord.y);
    }

    return vec2(localCoord.y, 1.0 - localCoord.x);
}

void main()
{
    WaterInstance instance = instanceBuffer.instanceData[gl_InstanceIndex];
    float leafSize = instance.leafParams.x;
    float waterLevel = instance.leafParams.y;
    uint terrainSliceIndex = uint(max(instance.leafParams.z, 0.0));
    bool hasTerrainSlice = instance.leafParams.w > 0.5;
    uint bandMask = (instance.packedMetadata >> 16u) & 0xFFFFu;
    uint edgeIndex = (instance.packedMetadata >> 8u) & 0x3u;
    uint cascadeCount = uint(max(water.waterParams.w, 0.0));

    vec2 localMeters = rotateBridgeLocalCoord(edgeIndex, inLocalCoord) * leafSize;
    vec3 position = vec3(
        instance.position.x + localMeters.x,
        instance.position.y + waterLevel,
        instance.position.z + localMeters.y);
    float viewDistance = length(position);
    vec2 worldXZ = water.cameraAndTime.xy + position.xz;
    float metersPerPixelAtView = metersPerPixel(viewDistance);

    float localDepth = water.depthEffectParams.x;
    if (hasTerrainSlice)
    {
        float terrainHeight = sampleTerrainHeight(terrainSliceIndex, localMeters, leafSize);
        localDepth = max(waterLevel - terrainHeight, 0.0);
    }

    float shoreFactor = 0.0;
    if (hasTerrainSlice)
    {
        float shoreDepth = max(water.depthEffectParams.z, 0.001);
        shoreFactor = 1.0 - smoothstep(0.0, shoreDepth, localDepth);
    }

    vec3 displacement = vec3(0.0);
    for (uint cascadeIndex = 0u; cascadeIndex < cascadeCount; ++cascadeIndex)
    {
        if ((bandMask & (1u << cascadeIndex)) == 0u)
        {
            continue;
        }

        float worldSize = max(cascadeWorldSize(cascadeIndex), 1.0);
        float detailWeight = cascadeDetailWeight(worldSize, metersPerPixelAtView);
        if (detailWeight <= 0.0)
        {
            continue;
        }

        vec2 uv = fract(worldXZ / worldSize);
        float dampingStrength = max(cascadeShallowDamping(cascadeIndex), 0.0);
        float shallowFade = hasTerrainSlice ? cascadeShallowFade(cascadeIndex, localDepth) : 1.0;
        float cascadeFade = mix(1.0, shallowFade, clamp(dampingStrength, 0.0, 8.0));
        displacement += texture(displacementTexture, vec3(uv, float(cascadeIndex))).xyz * (cascadeFade * detailWeight);
    }

    position += displacement;

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
