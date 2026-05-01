#version 450

layout(set=1, binding=0) uniform TerrainUniforms
{
    mat4 viewProjection;
    vec4 sunDirectionIntensity;
    vec4 sunColorAmbient;
    vec4 terrainHeightParams;
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

struct TerrainInstance
{
    vec3 position;
    uint packedMetadata;
};

layout(set=0, binding=1, std430) readonly buffer InstanceBuffer
{
    TerrainInstance instanceData[];
} instanceBuffer;

layout(location = 0) in vec2 inLocalCoord;
layout(location = 1) in vec2 inSampleCoord;

layout(location = 0) out vec3 fragLocalPosition;
layout(location = 1) out vec3 fragWorldNormal;
layout(location = 2) flat out uint fragAllowCaustics;

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

void main()
{
    TerrainInstance instance = instanceBuffer.instanceData[gl_InstanceIndex];
    uint sliceIndex = instance.packedMetadata & 0xFFFFu;
    uint scalePow = (instance.packedMetadata >> 16u) & 0xFFu;
    uint edgeIndex = (instance.packedMetadata >> 24u) & 0x3u;
    float leafSize = kMinimumQuadSize * exp2(float(scalePow));
    float sampleSpacing = leafSize / kHeightmapLeafIntervalCount;

    vec2 localCoord = vec2(0.0);
    ivec2 sampleCoord = ivec2(0);
    rotateBridgeCoords(edgeIndex, localCoord, sampleCoord);

    float height = sampleHeight(sliceIndex, sampleCoord);
    vec2 localOffset = localCoord * sampleSpacing;

    vec3 worldPosition = vec3(
        instance.position.x + localOffset.x,
        instance.position.y + height,
        instance.position.z + localOffset.y
    );

    gl_Position = terrain.viewProjection * vec4(worldPosition, 1.0);
    fragLocalPosition = worldPosition;
    fragWorldNormal = computeNormal(sliceIndex, sampleCoord, sampleSpacing);
    fragAllowCaustics = scalePow == 0u ? 1u : 0u;
}
