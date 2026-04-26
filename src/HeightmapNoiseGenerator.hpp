#pragma once

#include "AppConfig.hpp"
#include "Position.hpp"

#include <utility>
#include <cstdint>

struct TerrainFractalNoiseLayerSettings
{
    double wavelength = AppConfig::Terrain::kMediumDetailWavelength;
    double amplitude = AppConfig::Terrain::kMediumDetailAmplitude;
    double bias = AppConfig::Terrain::kMediumDetailBias;
    double initialFrequency = AppConfig::Terrain::kMediumDetailInitialFrequency;
    double initialAmplitude = AppConfig::Terrain::kMediumDetailInitialAmplitude;
    std::uint32_t octaveCount = AppConfig::Terrain::kMediumDetailOctaveCount;
    double octaveFrequencyScale = AppConfig::Terrain::kMediumDetailOctaveFrequencyScale;
    double octaveAmplitudeScale = AppConfig::Terrain::kMediumDetailOctaveAmplitudeScale;
    double gradientDampenStrength = AppConfig::Terrain::kMediumDetailGradientDampenStrength;
    double octaveRotationDegrees = AppConfig::Terrain::kMediumDetailOctaveRotationDegrees;
};

struct TerrainBlendNoiseSettings
{
    double wavelength = AppConfig::Terrain::kBlendWavelength;
    double initialFrequency = AppConfig::Terrain::kBlendInitialFrequency;
    double initialAmplitude = AppConfig::Terrain::kBlendInitialAmplitude;
    std::uint32_t octaveCount = AppConfig::Terrain::kBlendOctaveCount;
    double octaveFrequencyScale = AppConfig::Terrain::kBlendOctaveFrequencyScale;
    double octaveAmplitudeScale = AppConfig::Terrain::kBlendOctaveAmplitudeScale;
    double gradientDampenStrength = AppConfig::Terrain::kBlendGradientDampenStrength;
    double octaveRotationDegrees = AppConfig::Terrain::kBlendOctaveRotationDegrees;
    double lowThreshold = AppConfig::Terrain::kBlendLowThreshold;
    double highThreshold = AppConfig::Terrain::kBlendHighThreshold;
    double lowTransitionWidth = AppConfig::Terrain::kBlendLowTransitionWidth;
    double highTransitionWidth = AppConfig::Terrain::kBlendHighTransitionWidth;
};

struct TerrainNoiseSettings
{
    double baseHeight = AppConfig::Terrain::kBaseHeight;
    TerrainFractalNoiseLayerSettings hills{
        .wavelength = AppConfig::Terrain::kHillsWavelength,
        .amplitude = AppConfig::Terrain::kHillsAmplitude,
        .bias = AppConfig::Terrain::kHillsBias,
        .initialFrequency = AppConfig::Terrain::kHillsInitialFrequency,
        .initialAmplitude = AppConfig::Terrain::kHillsInitialAmplitude,
        .octaveCount = AppConfig::Terrain::kHillsOctaveCount,
        .octaveFrequencyScale = AppConfig::Terrain::kHillsOctaveFrequencyScale,
        .octaveAmplitudeScale = AppConfig::Terrain::kHillsOctaveAmplitudeScale,
        .gradientDampenStrength = AppConfig::Terrain::kHillsGradientDampenStrength,
        .octaveRotationDegrees = AppConfig::Terrain::kHillsOctaveRotationDegrees,
    };
    TerrainFractalNoiseLayerSettings mediumDetail{
        .wavelength = AppConfig::Terrain::kMediumDetailWavelength,
        .amplitude = AppConfig::Terrain::kMediumDetailAmplitude,
        .bias = AppConfig::Terrain::kMediumDetailBias,
        .initialFrequency = AppConfig::Terrain::kMediumDetailInitialFrequency,
        .initialAmplitude = AppConfig::Terrain::kMediumDetailInitialAmplitude,
        .octaveCount = AppConfig::Terrain::kMediumDetailOctaveCount,
        .octaveFrequencyScale = AppConfig::Terrain::kMediumDetailOctaveFrequencyScale,
        .octaveAmplitudeScale = AppConfig::Terrain::kMediumDetailOctaveAmplitudeScale,
        .gradientDampenStrength = AppConfig::Terrain::kMediumDetailGradientDampenStrength,
        .octaveRotationDegrees = AppConfig::Terrain::kMediumDetailOctaveRotationDegrees,
    };
    TerrainFractalNoiseLayerSettings highDetail{
        .wavelength = AppConfig::Terrain::kHighDetailWavelength,
        .amplitude = AppConfig::Terrain::kHighDetailAmplitude,
        .bias = AppConfig::Terrain::kHighDetailBias,
        .initialFrequency = AppConfig::Terrain::kHighDetailInitialFrequency,
        .initialAmplitude = AppConfig::Terrain::kHighDetailInitialAmplitude,
        .octaveCount = AppConfig::Terrain::kHighDetailOctaveCount,
        .octaveFrequencyScale = AppConfig::Terrain::kHighDetailOctaveFrequencyScale,
        .octaveAmplitudeScale = AppConfig::Terrain::kHighDetailOctaveAmplitudeScale,
        .gradientDampenStrength = AppConfig::Terrain::kHighDetailGradientDampenStrength,
        .octaveRotationDegrees = AppConfig::Terrain::kHighDetailOctaveRotationDegrees,
    };
    TerrainBlendNoiseSettings blend{};
};

[[nodiscard]] TerrainFractalNoiseLayerSettings sanitizeTerrainFractalNoiseLayerSettings(
    const TerrainFractalNoiseLayerSettings& settings);
[[nodiscard]] TerrainBlendNoiseSettings sanitizeTerrainBlendNoiseSettings(
    const TerrainBlendNoiseSettings& settings);
[[nodiscard]] TerrainNoiseSettings sanitizeTerrainNoiseSettings(const TerrainNoiseSettings& settings);
[[nodiscard]] double terrainNoiseMaxAmplitude(const TerrainNoiseSettings& settings);

class HeightmapNoiseGenerator
{
public:
    [[nodiscard]] TerrainNoiseSettings& settings() { return m_settings; }
    [[nodiscard]] const TerrainNoiseSettings& settings() const { return m_settings; }

    [[nodiscard]] std::pair<float, float> fillNoise(const Position& a, const Position& b, float* buffer = nullptr) const;

private:
    TerrainNoiseSettings m_settings{};
};
