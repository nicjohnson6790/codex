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
constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;

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

std::uint32_t hashNoise(std::int64_t x, std::int64_t z)
{
    std::uint32_t hash = static_cast<std::uint32_t>(x) * 0x8DA6B343U;
    hash ^= static_cast<std::uint32_t>(z) * 0xD8163841U;
    hash = (hash ^ (hash >> 13U)) * 0x85EBCA6BU;
    hash ^= hash >> 16U;
    return hash;
}

glm::dvec2 gradientFromHash(std::uint32_t hash)
{
    constexpr double kDiagonal = 0.7071067811865475244;
    switch (hash & 7U)
    {
    case 0: return { 1.0, 0.0 };
    case 1: return { -1.0, 0.0 };
    case 2: return { 0.0, 1.0 };
    case 3: return { 0.0, -1.0 };
    case 4: return { kDiagonal, kDiagonal };
    case 5: return { -kDiagonal, kDiagonal };
    case 6: return { kDiagonal, -kDiagonal };
    default: return { -kDiagonal, -kDiagonal };
    }
}

NoiseSample sampleGradientNoise(double x, double z)
{
    const std::int64_t baseX = static_cast<std::int64_t>(std::floor(x));
    const std::int64_t baseZ = static_cast<std::int64_t>(std::floor(z));
    const double fracX = x - static_cast<double>(baseX);
    const double fracZ = z - static_cast<double>(baseZ);
    const double smoothX = smoothStep(fracX);
    const double smoothZ = smoothStep(fracZ);
    const double derivativeX = smoothStepDerivative(fracX);
    const double derivativeZ = smoothStepDerivative(fracZ);

    const glm::dvec2 g00 = gradientFromHash(hashNoise(baseX, baseZ));
    const glm::dvec2 g10 = gradientFromHash(hashNoise(baseX + 1, baseZ));
    const glm::dvec2 g01 = gradientFromHash(hashNoise(baseX, baseZ + 1));
    const glm::dvec2 g11 = gradientFromHash(hashNoise(baseX + 1, baseZ + 1));

    const double v00 = glm::dot(g00, glm::dvec2(fracX, fracZ));
    const double v10 = glm::dot(g10, glm::dvec2(fracX - 1.0, fracZ));
    const double v01 = glm::dot(g01, glm::dvec2(fracX, fracZ - 1.0));
    const double v11 = glm::dot(g11, glm::dvec2(fracX - 1.0, fracZ - 1.0));

    const double ix0 = v00 + ((v10 - v00) * smoothX);
    const double ix1 = v01 + ((v11 - v01) * smoothX);
    const double value = ix0 + ((ix1 - ix0) * smoothZ);

    const double row0Dx = g00.x + ((g10.x - g00.x) * smoothX) + ((v10 - v00) * derivativeX);
    const double row1Dx = g01.x + ((g11.x - g01.x) * smoothX) + ((v11 - v01) * derivativeX);
    const double dValueDx = row0Dx + ((row1Dx - row0Dx) * smoothZ);

    const double row0Dz = g00.y + ((g10.y - g00.y) * smoothX);
    const double row1Dz = g01.y + ((g11.y - g01.y) * smoothX);
    const double dValueDz = row0Dz + ((row1Dz - row0Dz) * smoothZ) + ((ix1 - ix0) * derivativeZ);

    return {
        .value = value,
        .gradient = { dValueDx, dValueDz },
    };
}

double octaveNoise(double x, double z, const TerrainFractalNoiseLayerSettings& settings)
{
    const TerrainFractalNoiseLayerSettings sanitized = sanitizeTerrainFractalNoiseLayerSettings(settings);
    const double rotationRadians = sanitized.octaveRotationDegrees * kDegreesToRadians;
    const double rotationCosine = std::cos(rotationRadians);
    const double rotationSine = std::sin(rotationRadians);
    glm::dvec2 p{
        (rotationCosine * x) - (rotationSine * z),
        (rotationSine * x) + (rotationCosine * z)
    };
    p *= sanitized.initialFrequency;

    double value = 0.0;
    double amplitude = sanitized.initialAmplitude;
    double amplitudeSum = 0.0;
    glm::dvec2 accumulatedGradient{ 0.0, 0.0 };

    for (std::uint32_t octave = 0; octave < sanitized.octaveCount; ++octave)
    {
        amplitudeSum += amplitude;
        const NoiseSample sample = sampleGradientNoise(p.x, p.y);
        accumulatedGradient += sample.gradient;
        value +=
            (amplitude * sample.value) /
            (1.0 + (sanitized.gradientDampenStrength * glm::dot(accumulatedGradient, accumulatedGradient)));

        amplitude *= sanitized.octaveAmplitudeScale;
        p = glm::dvec2(
            (rotationCosine * p.x) - (rotationSine * p.y),
            (rotationSine * p.x) + (rotationCosine * p.y));
        p *= sanitized.octaveFrequencyScale;
    }

    return amplitudeSum > 0.0 ? (value / amplitudeSum) : 0.0;
}

double sampleFractalLayer(
    double worldX,
    double worldZ,
    const TerrainFractalNoiseLayerSettings& settings)
{
    const double sampleX = worldX / settings.wavelength;
    const double sampleZ = worldZ / settings.wavelength;
    return settings.bias + (octaveNoise(sampleX, sampleZ, settings) * settings.amplitude);
}

double sampleBlendChannel(
    double worldX,
    double worldZ,
    const TerrainBlendNoiseSettings& settings)
{
    const TerrainFractalNoiseLayerSettings blendAsLayer{
        .wavelength = settings.wavelength,
        .amplitude = 1.0,
        .bias = 0.0,
        .initialFrequency = settings.initialFrequency,
        .initialAmplitude = settings.initialAmplitude,
        .octaveCount = settings.octaveCount,
        .octaveFrequencyScale = settings.octaveFrequencyScale,
        .octaveAmplitudeScale = settings.octaveAmplitudeScale,
        .gradientDampenStrength = settings.gradientDampenStrength,
        .octaveRotationDegrees = settings.octaveRotationDegrees,
    };
    return 0.5 + (0.5 * sampleFractalLayer(worldX, worldZ, blendAsLayer));
}

double sampleHeight(double worldX, double worldZ, const TerrainNoiseSettings& settings)
{
    const double blendValue = sampleBlendChannel(worldX, worldZ, settings.blend);
    const double lowToMedium = smoothStep(std::clamp(
        (blendValue - settings.blend.lowThreshold) /
        std::max(std::abs(settings.blend.lowTransitionWidth), 0.0001),
        0.0,
        1.0));
    const double mediumToHigh = smoothStep(std::clamp(
        (blendValue - settings.blend.highThreshold) /
        std::max(std::abs(settings.blend.highTransitionWidth), 0.0001),
        0.0,
        1.0));

    const double hillsHeight = sampleFractalLayer(worldX, worldZ, settings.hills);
    const double mediumHeight = sampleFractalLayer(worldX, worldZ, settings.mediumDetail);
    const double highHeight = sampleFractalLayer(worldX, worldZ, settings.highDetail);

    return settings.baseHeight +
        hillsHeight +
        (mediumHeight * lowToMedium) +
        (highHeight * mediumToHigh);
}
}

TerrainFractalNoiseLayerSettings sanitizeTerrainFractalNoiseLayerSettings(
    const TerrainFractalNoiseLayerSettings& settings)
{
    TerrainFractalNoiseLayerSettings sanitized = settings;
    sanitized.wavelength = std::max(1.0, sanitized.wavelength);
    sanitized.amplitude = std::max(0.0, sanitized.amplitude);
    sanitized.initialFrequency = std::max(0.0001, sanitized.initialFrequency);
    sanitized.initialAmplitude = std::max(0.0, sanitized.initialAmplitude);
    sanitized.octaveCount = std::max<std::uint32_t>(1, sanitized.octaveCount);
    sanitized.octaveFrequencyScale = std::max(1.01, sanitized.octaveFrequencyScale);
    sanitized.octaveAmplitudeScale = std::clamp(sanitized.octaveAmplitudeScale, 0.0, 1.0);
    sanitized.gradientDampenStrength = std::max(0.0, sanitized.gradientDampenStrength);
    sanitized.octaveRotationDegrees = std::clamp(sanitized.octaveRotationDegrees, -180.0, 180.0);
    return sanitized;
}

TerrainBlendNoiseSettings sanitizeTerrainBlendNoiseSettings(
    const TerrainBlendNoiseSettings& settings)
{
    TerrainBlendNoiseSettings sanitized = settings;
    sanitized.wavelength = std::max(1.0, sanitized.wavelength);
    sanitized.initialFrequency = std::max(0.0001, sanitized.initialFrequency);
    sanitized.initialAmplitude = std::max(0.0, sanitized.initialAmplitude);
    sanitized.octaveCount = std::max<std::uint32_t>(1, sanitized.octaveCount);
    sanitized.octaveFrequencyScale = std::max(1.01, sanitized.octaveFrequencyScale);
    sanitized.octaveAmplitudeScale = std::clamp(sanitized.octaveAmplitudeScale, 0.0, 1.0);
    sanitized.gradientDampenStrength = std::max(0.0, sanitized.gradientDampenStrength);
    sanitized.octaveRotationDegrees = std::clamp(sanitized.octaveRotationDegrees, -180.0, 180.0);
    return sanitized;
}

TerrainNoiseSettings sanitizeTerrainNoiseSettings(const TerrainNoiseSettings& settings)
{
    TerrainNoiseSettings sanitized = settings;
    sanitized.hills = sanitizeTerrainFractalNoiseLayerSettings(sanitized.hills);
    sanitized.mediumDetail = sanitizeTerrainFractalNoiseLayerSettings(sanitized.mediumDetail);
    sanitized.highDetail = sanitizeTerrainFractalNoiseLayerSettings(sanitized.highDetail);
    sanitized.blend = sanitizeTerrainBlendNoiseSettings(sanitized.blend);
    return sanitized;
}

double terrainNoiseMaxAmplitude(const TerrainNoiseSettings& settings)
{
    const TerrainNoiseSettings sanitized = sanitizeTerrainNoiseSettings(settings);
    return std::max({
        sanitized.hills.amplitude,
        sanitized.mediumDetail.amplitude,
        sanitized.highDetail.amplitude,
    });
}

std::pair<float, float> HeightmapNoiseGenerator::fillNoise(const Position& a, const Position& b, float* buffer) const
{
    const TerrainNoiseSettings sanitized = sanitizeTerrainNoiseSettings(m_settings);
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
            const float height = static_cast<float>(sampleHeight(sampleX, sampleZ, sanitized));
            if (buffer != nullptr)
            {
                buffer[(static_cast<std::size_t>(z) * AppConfig::Terrain::kHeightmapResolution) + x] = height;
            }
            minHeight = std::min(minHeight, height);
            maxHeight = std::max(maxHeight, height);
        }
    }

    return { minHeight, maxHeight };
}
