#pragma once

#include "AppConfig.hpp"
#include "Position.hpp"

#include <utility>
#include <cstdint>

struct TerrainNoiseSettings
{
    double baseHeight = AppConfig::Terrain::kBaseHeight;
    double baseWavelength = 12000.0;
    double initialFrequency = 1.0;
    double initialAmplitude = 1.0;
    std::uint32_t octaveCount = 6;
    double octaveFrequencyScale = 2.0;
    double octaveAmplitudeScale = 0.5;
    double gradientDampenStrength = 0.85;
};

class HeightmapNoiseGenerator
{
public:
    [[nodiscard]] TerrainNoiseSettings& settings() { return m_settings; }
    [[nodiscard]] const TerrainNoiseSettings& settings() const { return m_settings; }

    [[nodiscard]] std::pair<float, float> fillNoise(const Position& a, const Position& b, float* buffer = nullptr) const;

private:
    TerrainNoiseSettings m_settings{};
};
