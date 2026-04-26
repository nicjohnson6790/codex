#include "HeightmapNoiseGenerator.hpp"

#include "AppConfig.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <limits>

namespace
{
constexpr double kOctaveRotationCosine = 0.8;
constexpr double kOctaveRotationSine = 0.6;

struct NoiseSample
{
    double value = 0.0;
    glm::dvec2 gradient{ 0.0, 0.0 };
};

double smoothStep(double t)
{
    return t * t * t * ((t * ((t * 6.0) - 15.0)) + 10.0);
}

double smoothStepDerivative(double t)
{
    return 30.0 * t * t * ((t * (t - 2.0)) + 1.0);
}

double valueNoise(std::int64_t x, std::int64_t z)
{
    std::uint32_t hash = static_cast<std::uint32_t>(x) * 0x8DA6B343U;
    hash ^= static_cast<std::uint32_t>(z) * 0xD8163841U;
    hash = (hash ^ (hash >> 13U)) * 0x85EBCA6BU;
    hash ^= hash >> 16U;
    return (static_cast<double>(hash & 0x00FFFFFFU) / static_cast<double>(0x00FFFFFFU)) * 2.0 - 1.0;
}

NoiseSample sampleValueNoise(double x, double z)
{
    const std::int64_t baseX = static_cast<std::int64_t>(std::floor(x));
    const std::int64_t baseZ = static_cast<std::int64_t>(std::floor(z));
    const double fracX = x - static_cast<double>(baseX);
    const double fracZ = z - static_cast<double>(baseZ);
    const double smoothX = smoothStep(fracX);
    const double smoothZ = smoothStep(fracZ);
    const double derivativeX = smoothStepDerivative(fracX);
    const double derivativeZ = smoothStepDerivative(fracZ);

    const double n00 = valueNoise(baseX, baseZ);
    const double n10 = valueNoise(baseX + 1, baseZ);
    const double n01 = valueNoise(baseX, baseZ + 1);
    const double n11 = valueNoise(baseX + 1, baseZ + 1);

    const double nx0 = n00 + ((n10 - n00) * smoothX);
    const double nx1 = n01 + ((n11 - n01) * smoothX);
    const double value = nx0 + ((nx1 - nx0) * smoothZ);

    const double dValueDx =
        (((n10 - n00) * (1.0 - smoothZ)) + ((n11 - n01) * smoothZ)) * derivativeX;
    const double dValueDz = (nx1 - nx0) * derivativeZ;

    return {
        .value = value,
        .gradient = { dValueDx, dValueDz },
    };
}

TerrainNoiseSettings sanitizedSettings(const TerrainNoiseSettings& settings)
{
    TerrainNoiseSettings sanitized = settings;
    sanitized.baseWavelength = std::max(1.0, sanitized.baseWavelength);
    sanitized.initialFrequency = std::max(0.0001, sanitized.initialFrequency);
    sanitized.initialAmplitude = std::max(0.0, sanitized.initialAmplitude);
    sanitized.octaveCount = std::max<std::uint32_t>(1, sanitized.octaveCount);
    sanitized.octaveFrequencyScale = std::max(1.01, sanitized.octaveFrequencyScale);
    sanitized.octaveAmplitudeScale = std::clamp(sanitized.octaveAmplitudeScale, 0.0, 1.0);
    sanitized.gradientDampenStrength = std::max(0.0, sanitized.gradientDampenStrength);
    return sanitized;
}

double octaveNoise(double x, double z, const TerrainNoiseSettings& settings)
{
    const TerrainNoiseSettings sanitized = sanitizedSettings(settings);
    glm::dvec2 p{ x * sanitized.initialFrequency, z * sanitized.initialFrequency };
    double value = 0.0;
    double amplitude = sanitized.initialAmplitude;
    glm::dvec2 accumulatedGradient{ 0.0, 0.0 };

    for (std::uint32_t octave = 0; octave < sanitized.octaveCount; ++octave)
    {
        const NoiseSample sample = sampleValueNoise(p.x, p.y);
        accumulatedGradient += sample.gradient;
        value +=
            (amplitude * sample.value) /
            (1.0 + (sanitized.gradientDampenStrength * glm::dot(accumulatedGradient, accumulatedGradient)));

        amplitude *= sanitized.octaveAmplitudeScale;
        p = glm::dvec2(
            (kOctaveRotationCosine * p.x) - (kOctaveRotationSine * p.y),
            (kOctaveRotationSine * p.x) + (kOctaveRotationCosine * p.y));
        p *= sanitized.octaveFrequencyScale;
    }

    return value;
}

double sampleHeight(double worldX, double worldZ, const TerrainNoiseSettings& settings)
{
    const TerrainNoiseSettings sanitized = sanitizedSettings(settings);
    const double sampleX = worldX / sanitized.baseWavelength;
    const double sampleZ = worldZ / sanitized.baseWavelength;
    return sanitized.baseHeight +
        (octaveNoise(sampleX, sampleZ, sanitized) * static_cast<double>(AppConfig::Terrain::kHeightAmplitude));
}
}

std::pair<float, float> HeightmapNoiseGenerator::fillNoise(const Position& a, const Position& b, float* buffer) const
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
    float minHeight = std::numeric_limits<float>::max();
    float maxHeight = std::numeric_limits<float>::lowest();

    for (std::uint32_t z = 0; z < AppConfig::Terrain::kHeightmapResolution; ++z)
    {
        const double sampleZ = worldMinZ + (stepZ * static_cast<double>(z));
        for (std::uint32_t x = 0; x < AppConfig::Terrain::kHeightmapResolution; ++x)
        {
            const double sampleX = worldMinX + (stepX * static_cast<double>(x));
            const float height = static_cast<float>(sampleHeight(sampleX, sampleZ, m_settings));
            buffer[(static_cast<std::size_t>(z) * AppConfig::Terrain::kHeightmapResolution) + x] = height;
            minHeight = std::min(minHeight, height);
            maxHeight = std::max(maxHeight, height);
        }
    }

    return { minHeight, maxHeight };
}
