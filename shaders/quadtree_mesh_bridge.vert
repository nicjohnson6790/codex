#version 450
#extension GL_GOOGLE_include_directive : require

layout(set=1, binding=0) uniform TerrainUniforms
{
    mat4 viewProjection;
    vec4 sunDirectionIntensity;
    vec4 sunColorAmbient;
    vec4 solarElevationParams;
    vec4 cameraWorldAndTime;
    vec4 waterCausticsParams;
    vec4 waterCascadeWorldSizesA;
    vec4 waterCascadeWorldSizesB;
    vec4 waterCausticsPatternParams;
    vec4 waterCausticsRidgeParamsA;
    vec4 waterCausticsRidgeParamsB;
    vec4 waterCausticsDecodeParams;
    vec4 waterCausticsRotationParams;
} terrain;

layout(set=0, binding=0, std430) readonly buffer HeightmapBuffer
{
    float heights[];
} heightmapBuffer;

#include "terrain_mesh_descriptors.glsl"

layout(location = 0) in vec2 inLocalCoord;
layout(location = 1) in vec2 inSampleCoord;

layout(location = 0) out vec3 fragLocalPosition;
layout(location = 1) out vec3 fragWorldNormal;
layout(location = 2) flat out uint fragAllowCaustics;
layout(location = 3) flat out uint fragIlluminationRegion;

const uint kHeightmapResolution = 259u;
const uint kHeightmapMaxCoord = kHeightmapResolution - 1u;
const float kHeightmapLeafIntervalCount = 256.0;
const float kMinimumQuadSize = 256.0;

float sampleHeight(uint sliceIndex, ivec2 sampleCoord)
{
    ivec2 clampedCoord = clamp(sampleCoord, ivec2(0), ivec2(int(kHeightmapMaxCoord)));
    uint linearIndex =
        (sliceIndex * kHeightmapResolution * kHeightmapResolution) +
        (uint(clampedCoord.y) * kHeightmapResolution) +
        uint(clampedCoord.x);
    return heightmapBuffer.heights[linearIndex];
}

vec3 computeNormal(uint sliceIndex, ivec2 sampleCoord, float sampleSpacing)
{
    ivec2 leftCoord = ivec2(max(sampleCoord.x - 1, 0), sampleCoord.y);
    ivec2 rightCoord = ivec2(min(sampleCoord.x + 1, int(kHeightmapMaxCoord)), sampleCoord.y);
    ivec2 downCoord = ivec2(sampleCoord.x, max(sampleCoord.y - 1, 0));
    ivec2 upCoord = ivec2(sampleCoord.x, min(sampleCoord.y + 1, int(kHeightmapMaxCoord)));

    float hL = sampleHeight(sliceIndex, leftCoord);
    float hR = sampleHeight(sliceIndex, rightCoord);
    float hD = sampleHeight(sliceIndex, downCoord);
    float hU = sampleHeight(sliceIndex, upCoord);

    float deltaX = sampleSpacing * float(max(rightCoord.x - leftCoord.x, 1));
    float deltaZ = sampleSpacing * float(max(upCoord.y - downCoord.y, 1));

    vec3 tangentX = vec3(deltaX, hR - hL, 0.0);
    vec3 tangentZ = vec3(0.0, hU - hD, deltaZ);
    return normalize(cross(tangentZ, tangentX));
}

void rotateBridgeCoords(uint edgeIndex, out vec2 localCoord, out ivec2 sampleCoord)
{
    vec2 baseSampleCoord = inSampleCoord;

    if (edgeIndex == 0u)
    {
        localCoord = inLocalCoord;
        sampleCoord = ivec2(baseSampleCoord);
        return;
    }

    if (edgeIndex == 1u)
    {
        localCoord = vec2(
            kHeightmapLeafIntervalCount - inLocalCoord.y,
            inLocalCoord.x);
        sampleCoord = ivec2(
            float(kHeightmapMaxCoord) - baseSampleCoord.y,
            baseSampleCoord.x);
        return;
    }

    if (edgeIndex == 2u)
    {
        localCoord = vec2(
            kHeightmapLeafIntervalCount - inLocalCoord.x,
            kHeightmapLeafIntervalCount - inLocalCoord.y);
        sampleCoord = ivec2(
            float(kHeightmapMaxCoord) - baseSampleCoord.x,
            float(kHeightmapMaxCoord) - baseSampleCoord.y);
        return;
    }

    localCoord = vec2(
        inLocalCoord.y,
        kHeightmapLeafIntervalCount - inLocalCoord.x);
    sampleCoord = ivec2(
        baseSampleCoord.y,
        float(kHeightmapMaxCoord) - baseSampleCoord.x);
}

ivec2 cornerSampleCoord(uint selector)
{
    // Row-major 3x3 perimeter, omitting the center: SW,S,SE,W,E,NW,N,NE.
    uint position = selector < 4u ? selector : selector + 1u;
    return ivec2(position % 3u, position / 3u) * 128 + ivec2(1);
}

ivec2 coarseOuterSampleCoord(uint edgeIndex, uint coarseHalf, vec2 localCoord)
{
    int along = int(round((edgeIndex == 0u || edgeIndex == 2u ? localCoord.y : localCoord.x) * 0.5));
    along += int(coarseHalf) * 128;
    if (edgeIndex == 0u)
        return ivec2(int(kHeightmapMaxCoord) - 1, 1 + along);
    if (edgeIndex == 1u)
        return ivec2(1 + along, int(kHeightmapMaxCoord) - 1);
    if (edgeIndex == 2u)
        return ivec2(1, 1 + along);
    return ivec2(1 + along, 1);
}

void main()
{
    uint reference = descriptors.bridges[gl_InstanceIndex];
    TerrainInstance instance = descriptors.parents[reference >> 2u].body;
    uvec4 edge = descriptors.parents[reference >> 2u].edges[reference & 3u];
    uvec4 heightmapIndices = uvec4(instance.packedMetadata & 0xFFFFu, edge.xyz);
    instance.packedMetadata = edge.w & 0x3FFFFFFFu;
    uint scalePow = (instance.packedMetadata >> 16u) & 0xFFu;
    uint edgeIndex = (instance.packedMetadata >> 24u) & 0x3u;
    uint coarseHalf = (instance.packedMetadata >> 26u) & 0x1u;
    float leafSize = kMinimumQuadSize * exp2(float(scalePow));
    float sampleSpacing = leafSize / kHeightmapLeafIntervalCount;

    vec2 localCoord = vec2(0.0);
    ivec2 sampleCoord = ivec2(0);
    rotateBridgeCoords(edgeIndex, localCoord, sampleCoord);

    bool outerVertex = inLocalCoord.x == 0.0;
    bool firstCorner = outerVertex && inLocalCoord.y == 0.0;
    bool secondCorner = outerVertex && inLocalCoord.y == kHeightmapLeafIntervalCount;
    uint sliceIndex = outerVertex ? heightmapIndices.y : heightmapIndices.x;
    float heightSampleSpacing = sampleSpacing;
    if (outerVertex && heightmapIndices.y != heightmapIndices.x)
    {
        sampleCoord = coarseOuterSampleCoord(edgeIndex, coarseHalf, localCoord);
        heightSampleSpacing *= 2.0;
    }
    if (firstCorner)
    {
        sliceIndex = heightmapIndices.z;
        sampleCoord = cornerSampleCoord(instance.packedMetadata & 7u);
        heightSampleSpacing = sliceIndex == heightmapIndices.x ? sampleSpacing : sampleSpacing * 2.0;
    }
    else if (secondCorner)
    {
        sliceIndex = heightmapIndices.w;
        sampleCoord = cornerSampleCoord((instance.packedMetadata >> 3u) & 7u);
        heightSampleSpacing = sliceIndex == heightmapIndices.x ? sampleSpacing : sampleSpacing * 2.0;
    }
    float height = sampleHeight(sliceIndex, sampleCoord);
    vec2 localOffset = localCoord * sampleSpacing;

    vec3 worldPosition = vec3(
        instance.position.x + localOffset.x,
        instance.position.y + height,
        instance.position.z + localOffset.y
    );

    gl_Position = terrain.viewProjection * vec4(worldPosition, 1.0);
    fragLocalPosition = worldPosition;
    fragIlluminationRegion = reference >> 2u;
    fragWorldNormal = computeNormal(sliceIndex, sampleCoord, heightSampleSpacing);
    fragAllowCaustics = scalePow == 0u ? 1u : 0u;
}
