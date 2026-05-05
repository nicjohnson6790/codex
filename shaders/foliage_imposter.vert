#version 450
#extension GL_ARB_shader_draw_parameters : require

layout(set=1, binding=0) uniform FoliageUniforms
{
    mat4 viewProjection;
} foliage;

struct FoliagePageData
{
    vec4 pageOriginAndTerrainSize;
    vec4 terrainOriginAndSlice;
    uvec4 seedData;
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

layout(location = 0) in float inEndpoint;

layout(location = 0) out vec3 fragColor;

const uint kCandidateGridResolution = 64u;
const float kCandidateCellSizeMeters = 4.0;
const uint kHeightmapResolution = 259u;
const float kHeightmapLeafIntervalCount = 256.0;

float hash01(uint seed)
{
    seed ^= seed >> 16u;
    seed *= 0x7feb352du;
    seed ^= seed >> 15u;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16u;
    return float(seed & 0x00FFFFFFu) / float(0x01000000u);
}

vec2 jitterOffset(uint seed)
{
    float jitterX = (hash01(seed ^ 0x68bc21ebu) * 2.0) - 1.0;
    float jitterZ = (hash01(seed ^ 0x02e5be93u) * 2.0) - 1.0;
    return vec2(jitterX, jitterZ) * 1.2;
}

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
    FoliagePageData page = drawMetadataBuffer.draws[gl_DrawIDARB];
    uint pageIndex = page.seedData.y;
    uint packedInstance =
        foliagePagePoolBuffer.packedInstances[
            (pageIndex * kCandidateGridResolution * kCandidateGridResolution) +
            uint(gl_InstanceIndex)];
    uint candidateSlot = packedInstance & 0x0FFFu;
    uint meshId = (packedInstance >> 12u) & 0xFFFFu;
    uint candidateX = candidateSlot & 63u;
    uint candidateZ = candidateSlot >> 6u;

    vec2 jitter = jitterOffset(page.seedData.x ^ candidateSlot);
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

    float markerHeight =
        meshId == 0u ? 8.0 :
        (meshId == 1u ? 14.0 : 22.0);
    if (length(vec3(
            pageOrigin.x + localOffset.x,
            pageOrigin.y + terrainHeight,
            pageOrigin.z + localOffset.y)) < 20.0)
    {
        markerHeight = 0.0;
    }
    vec3 worldPosition = vec3(
        pageOrigin.x + localOffset.x,
        pageOrigin.y + terrainHeight + (inEndpoint * markerHeight),
        pageOrigin.z + localOffset.y);
    gl_Position = foliage.viewProjection * vec4(worldPosition, 1.0);
    fragColor =
        meshId == 0u ? vec3(0.36, 0.82, 0.44) :
        (meshId == 1u ? vec3(0.58, 0.92, 0.46) : vec3(0.84, 0.94, 0.55));
}
