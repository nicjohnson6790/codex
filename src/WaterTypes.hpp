#pragma once

#include "AppConfig.hpp"
#include "Position.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <array>
#include <cstdint>

struct WaterCascadeSettings
{
    float worldSizeMeters = static_cast<float>(AppConfig::Water::kDefaultCascadeSizesMeters[0]);
    float amplitude = AppConfig::Water::kDefaultCascadeAmplitudes[0];
    float windDirectionRadians = AppConfig::Water::kDefaultCascadeWindDirectionsRadians[0];
    float windSpeed = AppConfig::Water::kDefaultCascadeWindSpeeds[0];
    float fetchMeters = AppConfig::Water::kDefaultCascadeFetchMeters[0];
    float spreadBlend = AppConfig::Water::kDefaultCascadeSpreadBlend[0];
    float swell = AppConfig::Water::kDefaultCascadeSwell[0];
    float peakEnhancement = AppConfig::Water::kDefaultCascadePeakEnhancement[0];
    float shortWavesFade = AppConfig::Water::kDefaultCascadeShortWavesFade[0];
    float choppiness = AppConfig::Water::kDefaultCascadeChoppiness[0];
    float shallowDampingStrength = AppConfig::Water::kDefaultCascadeShallowDampingStrength[0];
    std::uint32_t updateModulo = 1;
};

struct WaterSettings
{
    bool enabled = true;
    bool showLodTint = true;
    bool crestFoamEnabled = AppConfig::Water::kDefaultCrestFoamEnabled;
    float waterLevel = AppConfig::Water::kDefaultWaterLevel;
    float globalAmplitude = AppConfig::Water::kDefaultGlobalAmplitude;
    float globalChoppiness = AppConfig::Water::kDefaultGlobalChoppiness;
    float depthMeters = AppConfig::Water::kDefaultDepthMeters;
    float lowCutoff = AppConfig::Water::kDefaultLowCutoff;
    float highCutoff = AppConfig::Water::kDefaultHighCutoff;
    float crestFoamAmount = AppConfig::Water::kDefaultCrestFoamAmount;
    float crestFoamThreshold = AppConfig::Water::kDefaultCrestFoamThreshold;
    float crestFoamSoftness = AppConfig::Water::kDefaultCrestFoamSoftness;
    float crestFoamSlopeStart = AppConfig::Water::kDefaultCrestFoamSlopeStart;
    float crestFoamDecayRate = AppConfig::Water::kDefaultCrestFoamDecayRate;
    float crestFoamBrightness = AppConfig::Water::kDefaultCrestFoamBrightness;
    float maxTerrainMinHeightAboveWaterToDraw = AppConfig::Water::kMaxTerrainMinHeightAboveWaterToDraw;
    std::uint32_t cascadeCount = AppConfig::Water::kDefaultCascadeCount;
    std::array<WaterCascadeSettings, AppConfig::Water::kMaxCascadeCount> cascades{};
};

struct WaterLeafDrawRequest
{
    WorldGridQuadtreeLeafId leafId{};
    Position origin{};
    double sizeMeters = 0.0;
    std::uint8_t quadtreeLodHint = 0;
    std::uint32_t bandMask = 0;
    std::uint16_t terrainSliceIndex = 0;
    bool hasTerrainSlice = false;
};
