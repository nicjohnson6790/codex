#include "HeightmapNoiseGenerator.hpp"

#include "AppConfig.hpp"

#include <cmath>
#include <cstdint>

namespace
{
double smoothStep(double t)
{
    return t * t * (3.0 - (2.0 * t));
}

double valueNoise(std::int64_t x, std::int64_t z)
{
    std::uint32_t hash = static_cast<std::uint32_t>(x) * 0x8DA6B343U;
    hash ^= static_cast<std::uint32_t>(z) * 0xD8163841U;
    hash = (hash ^ (hash >> 13U)) * 0x85EBCA6BU;
    hash ^= hash >> 16U;
    return (static_cast<double>(hash & 0x00FFFFFFU) / static_cast<double>(0x00FFFFFFU)) * 2.0 - 1.0;
}

double octaveNoise(double x, double z)
{
    double value = 0.0;
    double amplitude = 1.0;
    double frequency = 1.0;
    double amplitudeSum = 0.0;

    for (std::uint32_t octave = 0; octave < 6; ++octave)
    {
        const double scaledX = x * frequency;
        const double scaledZ = z * frequency;
        const std::int64_t baseX = static_cast<std::int64_t>(std::floor(scaledX));
        const std::int64_t baseZ = static_cast<std::int64_t>(std::floor(scaledZ));
        const double smoothX = smoothStep(scaledX - static_cast<double>(baseX));
        const double smoothZ = smoothStep(scaledZ - static_cast<double>(baseZ));

        const double n00 = valueNoise(baseX, baseZ);
        const double n10 = valueNoise(baseX + 1, baseZ);
        const double n01 = valueNoise(baseX, baseZ + 1);
        const double n11 = valueNoise(baseX + 1, baseZ + 1);

        const double nx0 = n00 + ((n10 - n00) * smoothX);
        const double nx1 = n01 + ((n11 - n01) * smoothX);
        value += (nx0 + ((nx1 - nx0) * smoothZ)) * amplitude;
        amplitudeSum += amplitude;
        amplitude *= 0.58;
        frequency *= 2.35;
    }

    return amplitudeSum > 0.0 ? (value / amplitudeSum) : 0.0;
}

double sampleHeight(double worldX, double worldZ)
{
    const double sampleX = worldX * AppConfig::Terrain::kNoiseFrequency;
    const double sampleZ = worldZ * AppConfig::Terrain::kNoiseFrequency;
    return static_cast<double>(AppConfig::Terrain::kBaseHeight) +
        (octaveNoise(sampleX, sampleZ) * static_cast<double>(AppConfig::Terrain::kHeightAmplitude));
}
}

void HeightmapNoiseGenerator::fillNoise(const Position& a, const Position& b, float* buffer) const
{
    const double leafIntervalCount = static_cast<double>(AppConfig::Terrain::kHeightmapLeafIntervalCount);
    const double leafWorldMinX = a.worldPosition().x;
    const double leafWorldMinZ = a.worldPosition().z;
    const double leafWorldMaxX = b.worldPosition().x;
    const double leafWorldMaxZ = b.worldPosition().z;
    const double stepX = (leafWorldMaxX - leafWorldMinX) / leafIntervalCount;
    const double stepZ = (leafWorldMaxZ - leafWorldMinZ) / leafIntervalCount;
    const double worldMinX = leafWorldMinX - (stepX * static_cast<double>(AppConfig::Terrain::kHeightmapLeafHalo));
    const double worldMinZ = leafWorldMinZ - (stepZ * static_cast<double>(AppConfig::Terrain::kHeightmapLeafHalo));

    for (std::uint32_t z = 0; z < AppConfig::Terrain::kHeightmapResolution; ++z)
    {
        const double sampleZ = worldMinZ + (stepZ * static_cast<double>(z));
        for (std::uint32_t x = 0; x < AppConfig::Terrain::kHeightmapResolution; ++x)
        {
            const double sampleX = worldMinX + (stepX * static_cast<double>(x));
            buffer[(static_cast<std::size_t>(z) * AppConfig::Terrain::kHeightmapResolution) + x] =
                static_cast<float>(sampleHeight(sampleX, sampleZ));
        }
    }
}
