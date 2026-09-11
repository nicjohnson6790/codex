#version 450
#extension GL_ARB_shader_draw_parameters : require

#include "foliage_common.glsl"

layout(set=1, binding=0) uniform FoliageUniforms
{
    mat4 viewProjection;
    vec4 cameraPositionAndTreeClassCount;
} foliage;

struct FoliagePageData
{
    vec4 pageOriginAndTerrainSize;
    vec4 terrainOriginAndSlice;
    uvec4 seedData;
};

struct TreeClassData
{
    vec4 centerAndHalfWidth;
    vec4 verticalExtentsAndLayerBase;
    vec4 pitchHalfHeights;
    vec4 canopyCenterAndHalfExtents;
};

layout(set=0, binding=0, std430) readonly buffer FoliageDrawMetadataBuffer
{
    FoliagePageData draws[];
} drawMetadataBuffer;

layout(set=0, binding=1, std430) readonly buffer FoliagePagePoolBuffer
{
    uint packedInstances[];
} foliagePagePoolBuffer;

layout(set=0, binding=2, std430) readonly buffer HeightmapBuffer
{
    float heights[];
} heightmapBuffer;

layout(set=0, binding=3, std430) readonly buffer FoliageTreeClassBuffer
{
    TreeClassData treeClasses[];
} treeClassBuffer;

layout(location = 0) in vec2 inCorner;
layout(location = 1) in vec2 inUv0;

layout(location = 10) flat out uvec2 fragUpperLayers;
layout(location = 11) out vec2 fragUv1;
layout(location = 12) flat out float fragPitchBlend;
layout(location = 0) out vec2 fragUv0;
layout(location = 1) flat out uint fragLayerIndex0;
layout(location = 2) flat out uint fragLayerIndex1;
layout(location = 3) out float fragYawBlend;
layout(location = 4) out vec3 fragTreeRight;
layout(location = 5) out vec3 fragTreeUp;
layout(location = 6) out vec3 fragTreeForward;
layout(location = 7) out vec3 fragViewDirection;
layout(location = 8) out vec3 fragPosition;
layout(location = 9) flat out uint fragIlluminationRegion;

const uint kCandidateGridResolution = 64u;
const float kCandidateCellSizeMeters = 4.0;
const uint kHeightmapResolution = 259u;
const float kHeightmapLeafIntervalCount = 256.0;
const uint kImposterYawViewCount = 8u;
const uint kImposterLayersPerClass = 32u;
const float kTau = 6.28318530718;
const float kNearbyRadiusMeters = 100.0;
const float kPitchAngles[kImposterLayersPerClass / kImposterYawViewCount] = float[](radians(0.0), radians(25.0), radians(50.0), radians(75.0));

float sampleHeight(uint sliceIndex, ivec2 sampleCoord)
{
    ivec2 clampedCoord = clamp(sampleCoord, ivec2(0), ivec2(int(kHeightmapResolution - 1u)));
    uint linearIndex =
        (sliceIndex * kHeightmapResolution * kHeightmapResolution) +
        (uint(clampedCoord.y) * kHeightmapResolution) +
        uint(clampedCoord.x);
    return heightmapBuffer.heights[linearIndex];
}

float sampleHeightBilinear(uint sliceIndex, vec2 normalizedCoord)
{
    vec2 clampedCoord = clamp(normalizedCoord, vec2(0.0), vec2(1.0));
    vec2 sampleCoord = vec2(1.0) + (clampedCoord * kHeightmapLeafIntervalCount);
    ivec2 baseCoord = ivec2(floor(sampleCoord));
    vec2 fracCoord = fract(sampleCoord);

    float h00 = sampleHeight(sliceIndex, baseCoord);
    float h10 = sampleHeight(sliceIndex, baseCoord + ivec2(1, 0));
    float h01 = sampleHeight(sliceIndex, baseCoord + ivec2(0, 1));
    float h11 = sampleHeight(sliceIndex, baseCoord + ivec2(1, 1));

    float row0 = mix(h00, h10, fracCoord.x);
    float row1 = mix(h01, h11, fracCoord.x);
    return mix(row0, row1, fracCoord.y);
}

mat2 rotationMatrix(float radiansValue)
{
    float cosineValue = cos(radiansValue);
    float sineValue = sin(radiansValue);
    return mat2(
        cosineValue, -sineValue,
        sineValue, cosineValue);
}

void main()
{
    FoliagePageData page = drawMetadataBuffer.draws[gl_DrawIDARB];
    uint pageIndex = page.seedData.y;
    uint packedInstance =
        foliagePagePoolBuffer.packedInstances[
            (pageIndex * kCandidateGridResolution * kCandidateGridResolution) +
            uint(gl_InstanceIndex)];
    uint candidateSlot = packedInstance & 0x0FFFu;
    uint meshId = (packedInstance >> 12u) & 0x000Fu;
    uint rotationBits = (packedInstance >> 16u) & 0x0FFFu;

    if (float(meshId) >= foliage.cameraPositionAndTreeClassCount.w)
    {
        gl_Position = vec4(0.0);
        fragUv0 = inUv0;
        fragUv1 = inUv0;
        fragUpperLayers=uvec2(0);
        fragPitchBlend=0;
        fragPosition=vec3(0);
        fragIlluminationRegion=65535u;
        fragLayerIndex0 = 0u;
        fragLayerIndex1 = 0u;
        fragYawBlend = 0.0;
        fragTreeRight = vec3(1.0, 0.0, 0.0);
        fragTreeUp = vec3(0.0, 1.0, 0.0);
        fragTreeForward = vec3(0.0, 0.0, 1.0);
        fragViewDirection = vec3(0.0, 0.0, 1.0);
        return;
    }

    TreeClassData treeClass = treeClassBuffer.treeClasses[meshId];
    uint candidateX = candidateSlot & 63u;
    uint candidateZ = candidateSlot >> 6u;
    vec2 jitter = foliageJitterOffset(foliageCandidateSeed(page.seedData.x, candidateSlot));
    vec2 localOffset = vec2(
        (float(candidateX) + 0.5) * kCandidateCellSizeMeters,
        (float(candidateZ) + 0.5) * kCandidateCellSizeMeters) + jitter;

    vec3 terrainOrigin = page.terrainOriginAndSlice.xyz;
    vec3 pageOrigin = page.pageOriginAndTerrainSize.xyz;
    float terrainLeafSizeMeters = page.pageOriginAndTerrainSize.w;
    vec2 normalizedTerrainCoord = clamp(
        ((pageOrigin.xz + localOffset) - terrainOrigin.xz) / terrainLeafSizeMeters,
        vec2(0.0),
        vec2(1.0));
    uint terrainSliceIndex = uint(page.terrainOriginAndSlice.w + 0.5);
    float terrainHeight = sampleHeightBilinear(terrainSliceIndex, normalizedTerrainCoord);

    float instanceRotation = (float(rotationBits) / 4096.0) * kTau;
    mat2 instanceRotationMatrix = rotationMatrix(instanceRotation);
    vec2 rotatedCenterXz = instanceRotationMatrix * treeClass.centerAndHalfWidth.xz;
    vec3 instanceCenter = vec3(
        pageOrigin.x + localOffset.x + rotatedCenterXz.x,
        pageOrigin.y + terrainHeight + treeClass.centerAndHalfWidth.y,
        pageOrigin.z + localOffset.y + rotatedCenterXz.y);

    vec3 cameraPosition = foliage.cameraPositionAndTreeClassCount.xyz;
    vec3 deltaCamera = cameraPosition - instanceCenter;
    vec3 toCamera = length(deltaCamera)>0.0001 ? normalize(deltaCamera) : vec3(0,1,0);
    vec3 billboardForward = vec3(toCamera.x, 0.0, toCamera.z);
    if (length(billboardForward.xz) < 0.0001)
    {
        billboardForward = vec3(0.0, 0.0, -1.0);
    }
    billboardForward = normalize(billboardForward);
    vec3 billboardRight = normalize(vec3(billboardForward.z, 0.0, -billboardForward.x));
    vec3 billboardUp = vec3(0.0, 1.0, 0.0);

    vec3 localToCamera = normalize(vec3(
        (rotationMatrix(-instanceRotation) * toCamera.xz).x,
        toCamera.y,
        (rotationMatrix(-instanceRotation) * toCamera.xz).y));
    vec3 captureForward = -localToCamera;

    float yawRadians = dot(captureForward.xz,captureForward.xz)>1e-8 ? atan(captureForward.x, captureForward.z) : 0.0;
    if (yawRadians < 0.0)
    {
        yawRadians += kTau;
    }
    float yawPosition = (yawRadians / kTau) * float(kImposterYawViewCount);
    uint yawIndex0 = uint(floor(yawPosition)) % kImposterYawViewCount;
    uint yawIndex1 = (yawIndex0 + 1u) % kImposterYawViewCount;
    fragYawBlend = fract(yawPosition);

    float elevation = clamp(asin(clamp(toCamera.y,-1.0,1.0)),0.0,kPitchAngles[3]);
    float pitchPosition=elevation/kPitchAngles[1];
    uint pitchIndex0=min(uint(floor(pitchPosition)),3u);
    uint pitchIndex1=min(pitchIndex0+1u,3u);
    fragPitchBlend=pitchIndex0==pitchIndex1 ? 0.0 : fract(pitchPosition);
    billboardUp=vec3(-billboardForward.x*sin(elevation),cos(elevation),-billboardForward.z*sin(elevation));
    // Captured normals are tree-local at every pitch and yaw.
    // Only instance rotation maps them to world space.
    fragTreeRight=vec3(instanceRotationMatrix[0].x,0,instanceRotationMatrix[0].y);
    fragTreeUp=vec3(0,1,0);
    fragTreeForward=vec3(instanceRotationMatrix[1].x,0,instanceRotationMatrix[1].y);
    fragViewDirection = toCamera;

    float imposterScale = 1.0;
    vec2 rootToCamera=pageOrigin.xz+localOffset-cameraPosition.xz;
    if (dot(rootToCamera, rootToCamera) < (kNearbyRadiusMeters * kNearbyRadiusMeters))
    {
        imposterScale = 0.0;
    }

    float halfWidth = treeClass.centerAndHalfWidth.w;
    // Enclose both capture rectangles. Map physical billboard coordinates back
    // into each capture's own framing rather than stretching mismatched UVs.
    float halfHeight=max(treeClass.pitchHalfHeights[pitchIndex0],treeClass.pitchHalfHeights[pitchIndex1]);
    float verticalOffset=(inCorner.y*2.0-1.0)*halfHeight;
    vec3 worldPosition =
        instanceCenter +
        (billboardRight * (inCorner.x * halfWidth * imposterScale)) +
        (billboardUp * verticalOffset * imposterScale);

    fragPosition=worldPosition;
    fragIlluminationRegion=page.seedData.w;
    gl_Position = foliage.viewProjection * vec4(worldPosition, 1.0);
    fragUv0=vec2(inUv0.x,0.5+(inUv0.y-0.5)*halfHeight/treeClass.pitchHalfHeights[pitchIndex0]);
    fragUv1=vec2(inUv0.x,0.5+(inUv0.y-0.5)*halfHeight/treeClass.pitchHalfHeights[pitchIndex1]);
    uint upperBase=uint(treeClass.verticalExtentsAndLayerBase.z)+pitchIndex1*kImposterYawViewCount;
    fragUpperLayers=uvec2(upperBase+yawIndex0,upperBase+yawIndex1);
    uint layerBase = uint(treeClass.verticalExtentsAndLayerBase.z) + (pitchIndex0 * kImposterYawViewCount);
    fragLayerIndex0 = layerBase + yawIndex0;
    fragLayerIndex1 = layerBase + yawIndex1;
}
