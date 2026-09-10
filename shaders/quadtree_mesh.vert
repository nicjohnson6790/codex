#version 450
#extension GL_GOOGLE_include_directive : require

layout(set=1, binding=0) uniform TerrainUniforms
{
    mat4 viewProjection;
    vec4 sunDirectionIntensity;
    vec4 sunColorAmbient;
    vec4 reservedTerrainParams;
    vec4 cameraWorldAndTime;
    vec4 waterCausticsParams;
    vec4 waterCascadeWorldSizesA;
    vec4 waterCascadeWorldSizesB;
    vec4 waterCausticsPatternParams;
    vec4 waterCausticsRidgeParamsA;
    vec4 waterCausticsRidgeParamsB;
    vec4 waterCausticsDecodeParams;
    vec4 waterCausticsRotationParams;
    vec4 terrainOriginPhasesA;
    vec4 terrainOriginPhasesB;
    vec4 waterCascadeOriginPhasesA;
    vec4 waterCascadeOriginPhasesB;
    vec4 waterCausticsOriginPhases;
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

void main()
{
    TerrainInstance instance = descriptors.parents[gl_InstanceIndex].body;
    uint sliceIndex = instance.packedMetadata & 0xFFFFu;
    uint scalePow = (instance.packedMetadata >> 16u) & 0xFFu;
    float leafSize = kMinimumQuadSize * exp2(float(scalePow));
    float sampleSpacing = leafSize / kHeightmapLeafIntervalCount;
    vec2 localOffset = vec2(inLocalCoord.x * sampleSpacing, inLocalCoord.y * sampleSpacing);
    ivec2 sampleCoord = ivec2(inSampleCoord);

    float height = sampleHeight(sliceIndex, sampleCoord);

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
