float foliageHash01(uint seed)
{
    seed ^= seed >> 16u;
    seed *= 0x7feb352du;
    seed ^= seed >> 15u;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16u;
    return float(seed & 0x00FFFFFFu) / float(0x01000000u);
}

const float kFoliageBaseDensity = 0.08;
const float kFoliageClusterDensity = 0.18;
const float kFoliageClusterScale = 1.0 / 96.0;
const float kFoliageMinHeightAboveWater = 5.0;
const float kFoliageThinStartHeight = 850.0;
const float kFoliageMaxHeight = 2000.0;

float foliageFract(float value)
{
    return value - floor(value);
}

float foliageDensityAt(vec2 worldSample, float terrainHeight, float waterLevel)
{
    if (terrainHeight <= (waterLevel + kFoliageMinHeightAboveWater) ||
        terrainHeight >= kFoliageMaxHeight)
    {
        return 0.0;
    }

    float clusterNoise = foliageFract(
        sin((worldSample.x * kFoliageClusterScale) +
            (worldSample.y * (kFoliageClusterScale * 1.37)) +
            (terrainHeight * 0.0035)) *
        43758.5453);
    float elevationDensityScale =
        1.0 - smoothstep(kFoliageThinStartHeight, kFoliageMaxHeight, terrainHeight);
    return clamp(kFoliageBaseDensity + (clusterNoise * kFoliageClusterDensity), 0.0, 0.9) *
        elevationDensityScale;
}

bool foliageCandidateAccepted(uint slotSeed, vec2 worldSample, float terrainHeight, float waterLevel)
{
    float density = foliageDensityAt(worldSample, terrainHeight, waterLevel);
    return density > 0.0 && foliageHash01(slotSeed) <= density;
}

uint foliageCandidateSeed(uint leafSeed, uint candidateSlot)
{
    return leafSeed ^ (candidateSlot * 0x9e3779b9u);
}

vec2 foliageJitterOffset(uint seed)
{
    float jitterX = (foliageHash01(seed ^ 0x68bc21ebu) * 2.0) - 1.0;
    float jitterZ = (foliageHash01(seed ^ 0x02e5be93u) * 2.0) - 1.0;
    return vec2(jitterX, jitterZ) * 1.2;
}
