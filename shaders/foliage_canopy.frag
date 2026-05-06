#version 450

layout(set=3, binding=0) uniform FoliageCanopyUniforms
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

layout(set=2, binding=0, std430) readonly buffer CanopyDrawMetadataBuffer
{
    CanopyDrawData draws[];
} drawMetadataBuffer;

layout(set=2, binding=1, std430) readonly buffer CanopyBitsetPoolBuffer
{
    uint canopyBitsets[];
} canopyBitsetPoolBuffer;

layout(location = 0) in vec2 fragPatchMeters;
layout(location = 1) in vec2 fragWorldXZ;
layout(location = 2) flat in uint fragDrawIndex;

layout(location = 0) out vec4 outColor;

const float kCellSizeMeters = 256.0;
const float kCandidateCellSizeMeters = 4.0;
const float kEdgeFadeWidthMeters = 384.0;
const float kFadeInFrameCount = 12.0;
const uint kCandidateGridResolution = 64u;
const uint kCanopyBitsetWordCount = (kCandidateGridResolution * kCandidateGridResolution) / 32u;
const vec3 kPalette[4] = vec3[](
    vec3(0.16, 0.31, 0.14),
    vec3(0.21, 0.37, 0.16),
    vec3(0.25, 0.43, 0.20),
    vec3(0.18, 0.35, 0.19));

float hash01(uint seed)
{
    seed ^= seed >> 16u;
    seed *= 0x7feb352du;
    seed ^= seed >> 15u;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16u;
    return float(seed & 0x00FFFFFFu) / float(0x01000000u);
}

uint hashU32(uint seed)
{
    seed ^= seed >> 16u;
    seed *= 0x7feb352du;
    seed ^= seed >> 15u;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16u;
    return seed;
}

vec2 jitterOffset(uint seed)
{
    float jitterX = (hash01(seed ^ 0x68bc21ebu) * 2.0) - 1.0;
    float jitterZ = (hash01(seed ^ 0x02e5be93u) * 2.0) - 1.0;
    return vec2(jitterX, jitterZ) * 1.2;
}

bool canopyCandidateResident(uint cellSlotIndex, uint candidateSlot)
{
    if (cellSlotIndex == 0xFFFFFFFFu)
    {
        return false;
    }

    uint wordIndex = candidateSlot >> 5u;
    uint bitMask = 1u << (candidateSlot & 31u);
    uint residentWord = canopyBitsetPoolBuffer.canopyBitsets[(cellSlotIndex * kCanopyBitsetWordCount) + wordIndex];
    return (residentWord & bitMask) != 0u;
}

float unpackEdgeFadeStrength(uint packedStrengths, uint edgeIndex)
{
    return float((packedStrengths >> (edgeIndex * 8u)) & 0xFFu) / 255.0;
}

void main()
{
    CanopyDrawData draw = drawMetadataBuffer.draws[fragDrawIndex];
    vec2 clampedPatchMeters = clamp(fragPatchMeters, vec2(0.0), vec2(draw.patchOriginAndSize.w));
    vec2 cellCoord = min(floor(clampedPatchMeters / kCellSizeMeters), vec2(7.0));
    uvec2 cellIndex = uvec2(cellCoord);
    uint flatCellIndex = (cellIndex.y * 8u) + cellIndex.x;
    uint packedCellIndex = flatCellIndex >> 2u;
    uint packedCellLane = flatCellIndex & 3u;
    uint cellSlotIndex = draw.cellSlots[packedCellIndex][packedCellLane];
    uint cellSeed = draw.cellSeeds[packedCellIndex][packedCellLane];

    vec2 localCellMeters = clampedPatchMeters - (vec2(cellIndex) * kCellSizeMeters);
    vec2 candidateCoord = clamp((localCellMeters / kCandidateCellSizeMeters) - vec2(0.5), vec2(0.0), vec2(63.0));
    ivec2 baseCandidate = ivec2(floor(candidateCoord));
    ivec2 nextCandidate = min(baseCandidate + ivec2(1), ivec2(63));

    ivec2 candidates[4] = ivec2[](
        ivec2(baseCandidate.x, baseCandidate.y),
        ivec2(nextCandidate.x, baseCandidate.y),
        ivec2(baseCandidate.x, nextCandidate.y),
        ivec2(nextCandidate.x, nextCandidate.y));

    bool covered = false;
    float bestShape = 0.0;
    vec3 bestNormal = vec3(0.0, 1.0, 0.0);
    vec3 bestColor = kPalette[0];

    for (uint candidateIndex = 0u; candidateIndex < 4u; ++candidateIndex)
    {
        ivec2 candidateGrid = candidates[candidateIndex];
        uint candidateSlot = uint(candidateGrid.y * int(kCandidateGridResolution) + candidateGrid.x);
        if (!canopyCandidateResident(cellSlotIndex, candidateSlot))
        {
            continue;
        }

        uint slotSeed = cellSeed ^ (candidateSlot * 0x9e3779b9u);
        vec2 crownCenter =
            vec2(candidateGrid) * kCandidateCellSizeMeters +
            vec2(kCandidateCellSizeMeters * 0.5) +
            jitterOffset(slotSeed);
        float radius = mix(5.5, 11.5, hash01(slotSeed ^ 0x31f123bbu));
        vec2 delta = localCellMeters - crownCenter;
        float normalizedDistance = length(delta) / radius;
        if (normalizedDistance > 1.0)
        {
            continue;
        }

        float hemisphere = sqrt(max(0.0, 1.0 - (normalizedDistance * normalizedDistance)));
        vec3 normal = normalize(vec3(delta.x / max(radius, 0.001), hemisphere * 1.65, delta.y / max(radius, 0.001)));
        if (!covered || hemisphere > bestShape)
        {
            covered = true;
            bestShape = hemisphere;
            bestNormal = normal;
            bestColor = kPalette[(slotSeed >> 3u) & 3u];
        }
    }

    if (!covered)
    {
        discard;
    }

    vec3 lightDirection = normalize(canopy.sunDirectionIntensity.xyz);
    float diffuse = clamp(dot(bestNormal, lightDirection), 0.0, 1.0);
    float ambient = 0.42;
    float lighting = ambient + (diffuse * 0.58);
    vec3 color = bestColor * lighting * canopy.sunDirectionIntensity.w;

    uint edgeFadeStrengths = draw.patchSeedData.y;
    float edgeFade = 1.0;
    float westStrength = unpackEdgeFadeStrength(edgeFadeStrengths, 0u);
    if (westStrength > 0.0)
    {
        float westBand = clamp(clampedPatchMeters.x / kEdgeFadeWidthMeters, 0.0, 1.0);
        edgeFade = min(edgeFade, mix(1.0, westBand, westStrength));
    }
    float southStrength = unpackEdgeFadeStrength(edgeFadeStrengths, 1u);
    if (southStrength > 0.0)
    {
        float southBand = clamp(clampedPatchMeters.y / kEdgeFadeWidthMeters, 0.0, 1.0);
        edgeFade = min(edgeFade, mix(1.0, southBand, southStrength));
    }
    float eastStrength = unpackEdgeFadeStrength(edgeFadeStrengths, 2u);
    if (eastStrength > 0.0)
    {
        float eastBand = clamp((draw.patchOriginAndSize.w - clampedPatchMeters.x) / kEdgeFadeWidthMeters, 0.0, 1.0);
        edgeFade = min(edgeFade, mix(1.0, eastBand, eastStrength));
    }
    float northStrength = unpackEdgeFadeStrength(edgeFadeStrengths, 3u);
    if (northStrength > 0.0)
    {
        float northBand = clamp((draw.patchOriginAndSize.w - clampedPatchMeters.y) / kEdgeFadeWidthMeters, 0.0, 1.0);
        edgeFade = min(edgeFade, mix(1.0, northBand, northStrength));
    }

    float fadeIn = clamp(float(draw.patchSeedData.z) / kFadeInFrameCount, 0.0, 1.0);
    float combinedFade = min(edgeFade, fadeIn);

    ivec2 fadeCell = ivec2(floor(fragPatchMeters * 0.125));
    uint fadeSeed =
        draw.patchSeedData.x ^
        (uint(fadeCell.x) * 0x9e3779b9u) ^
        (uint(fadeCell.y) * 0x85ebca6bu) ^
        (flatCellIndex * 0xc2b2ae35u);
    if (hash01(hashU32(fadeSeed)) > combinedFade)
    {
        discard;
    }

    outColor = vec4(color, 1.0);
}
