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

ivec2 cornerSampleCoord(uint selector)
{
    uint position = selector < 4u ? selector : selector + 1u;
    return ivec2(position % 3u, position / 3u) * 128 + ivec2(1);
}

// Evaluate an exact endpoint first, including independently resolved corners.
void boundaryEndpoint(uvec4 edge, uint ownSlice, uint edgeIndex, int along,
    float spacing, out float height, out vec3 normal)
{
    bool coarse = (edge.w & 0x80000000u) != 0u;
    uint slice = edge.x;
    int ownerAlong = coarse ? along / 2 + int((edge.w >> 26u) & 1u) * 128 : along;
    int side = coarse ? (edgeIndex < 2u ? 257 : 1) : (edgeIndex < 2u ? 1 : 257);
    ivec2 coord = (edgeIndex == 0u || edgeIndex == 2u)
        ? ivec2(side, 1 + ownerAlong) : ivec2(1 + ownerAlong, side);
    float pitch = coarse ? spacing * 2.0 : spacing;
    if (along == 0 || along == 256)
    {
        bool first = (edgeIndex == 0u || edgeIndex == 3u) ? along == 0 : along == 256;
        slice = first ? edge.y : edge.z;
        coord = cornerSampleCoord(first ? edge.w & 7u : (edge.w >> 3u) & 7u);
        pitch = slice == ownSlice ? spacing : spacing * 2.0;
    }
    height = sampleHeight(slice, coord);
    normal = computeNormal(slice, coord, pitch);
}

void main()
{
    // Indexed draws retain vertex reuse with the original row-major vertex IDs.
    uint vertexIndex = uint(gl_VertexIndex);
    ivec2 gridCoord = ivec2(vertexIndex % 257u, vertexIndex / 257u);
    vec2 inLocalCoord = vec2(gridCoord);
    TerrainInstance instance = descriptors.parents[gl_InstanceIndex].body;
    uint sliceIndex = instance.packedMetadata & 0xFFFFu;
    uint scalePow = (instance.packedMetadata >> 16u) & 0xFFu;
    float leafSize = kMinimumQuadSize * exp2(float(scalePow));
    float sampleSpacing = leafSize / kHeightmapLeafIntervalCount;
    vec2 localOffset = vec2(inLocalCoord.x * sampleSpacing, inLocalCoord.y * sampleSpacing);
    ivec2 sampleCoord = gridCoord + ivec2(1);

    float height;
    vec3 normal;
    if (inLocalCoord.x == 0.0 || inLocalCoord.y == 0.0 ||
        inLocalCoord.x == 256.0 || inLocalCoord.y == 256.0)
    {
        uint edgeIndex = inLocalCoord.x == 0.0 ? 0u : inLocalCoord.y == 0.0 ? 1u :
            inLocalCoord.x == 256.0 ? 2u : 3u;
        int along = int(edgeIndex == 0u || edgeIndex == 2u ? inLocalCoord.y : inLocalCoord.x);
        uvec4 edge = descriptors.parents[gl_InstanceIndex].edges[edgeIndex];
        bool midpoint = (edge.w & 0x80000000u) != 0u && (along & 1) != 0;
        boundaryEndpoint(edge, sliceIndex, edgeIndex, midpoint ? along - 1 : along, sampleSpacing, height, normal);
        if (midpoint)
        {
            float nextHeight;
            vec3 nextNormal;
            boundaryEndpoint(edge, sliceIndex, edgeIndex, along + 1, sampleSpacing, nextHeight, nextNormal);
            height = (height + nextHeight) * 0.5;
            // Preserve the coarse edge's linear varying; the fragment shader
            // normalizes after interpolation, just as it does for the owner.
            normal = (normal + nextNormal) * 0.5;
        }
    }
    else
    {
        height = sampleHeight(sliceIndex, sampleCoord);
        normal = computeNormal(sliceIndex, sampleCoord, sampleSpacing);
    }

    vec3 worldPosition = vec3(
        instance.position.x + localOffset.x,
        instance.position.y + height,
        instance.position.z + localOffset.y
    );

    gl_Position = terrain.viewProjection * vec4(worldPosition, 1.0);
    fragLocalPosition = worldPosition;
    fragIlluminationRegion = gl_InstanceIndex;
    fragWorldNormal = normal;
    fragAllowCaustics = scalePow == 0u ? 1u : 0u;
}
