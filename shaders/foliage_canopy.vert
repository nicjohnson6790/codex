#version 450

layout(set=1, binding=0) uniform FoliageCanopyUniforms
{
    mat4 viewProjection;
    vec4 sunDirectionIntensity;
} canopy;

struct CanopyDrawData
{
    vec4 patchOriginAndSize;
    vec4 terrainOriginAndSize;
    vec4 terrainSliceData;
    uvec4 patchSeedData;
    uvec4 cellSlots[16];
    uvec4 cellSeeds[16];
};

layout(set=0, binding=0, std430) readonly buffer CanopyDrawMetadataBuffer
{
    CanopyDrawData draws[];
} drawMetadataBuffer;

layout(set=0, binding=1, std430) readonly buffer CanopyBitsetPoolBuffer
{
    uint canopyBitsets[];
} canopyBitsetPoolBuffer;

layout(set=0, binding=2, std430) readonly buffer HeightmapBuffer
{
    float heights[];
} heightmapBuffer;

layout(location = 0) in vec2 inUv;

layout(location = 0) out vec2 fragPatchMeters;
layout(location = 1) out vec2 fragWorldXZ;
layout(location = 2) flat out uint fragDrawIndex;

const uint kHeightmapResolution = 259u;
const float kHeightmapLeafIntervalCount = 256.0;
const float kCanopyBaseHeightMeters = 12.0;

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

void main()
{
    CanopyDrawData draw = drawMetadataBuffer.draws[gl_InstanceIndex];
    vec2 patchMeters = inUv * draw.patchOriginAndSize.w;
    vec2 normalizedTerrainCoord = clamp(patchMeters / draw.terrainOriginAndSize.w, vec2(0.0), vec2(1.0));
    float terrainHeight = sampleHeightBilinear(uint(draw.terrainSliceData.x + 0.5), normalizedTerrainCoord);
    vec3 worldPosition = vec3(
        draw.patchOriginAndSize.x + patchMeters.x,
        draw.patchOriginAndSize.y + terrainHeight + kCanopyBaseHeightMeters,
        draw.patchOriginAndSize.z + patchMeters.y);

    fragPatchMeters = patchMeters;
    fragWorldXZ = worldPosition.xz;
    fragDrawIndex = uint(gl_InstanceIndex);
    gl_Position = canopy.viewProjection * vec4(worldPosition, 1.0);
}
