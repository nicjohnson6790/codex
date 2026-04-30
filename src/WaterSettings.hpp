#pragma once

#include "AppConfig.hpp"
#include "WaterTypes.hpp"

inline WaterSettings makeDefaultWaterSettings()
{
    WaterSettings settings{};
    settings.enabled = AppConfig::Water::kEnabled;
    settings.showLodTint = true;
    settings.waterLevel = AppConfig::Water::kDefaultWaterLevel;
    settings.globalAmplitude = AppConfig::Water::kDefaultGlobalAmplitude;
    settings.globalChoppiness = AppConfig::Water::kDefaultGlobalChoppiness;
    settings.depthMeters = AppConfig::Water::kDefaultDepthMeters;
    settings.lowCutoff = AppConfig::Water::kDefaultLowCutoff;
    settings.highCutoff = AppConfig::Water::kDefaultHighCutoff;
    settings.maxTerrainMinHeightAboveWaterToDraw = AppConfig::Water::kMaxTerrainMinHeightAboveWaterToDraw;
    settings.cascadeCount = AppConfig::Water::kDefaultCascadeCount;

    for (std::uint32_t i = 0; i < settings.cascadeCount; ++i)
    {
        settings.cascades[i].worldSizeMeters = static_cast<float>(AppConfig::Water::kDefaultCascadeSizesMeters[i]);
        settings.cascades[i].amplitude = AppConfig::Water::kDefaultCascadeAmplitudes[i];
        settings.cascades[i].windDirectionRadians = AppConfig::Water::kDefaultWindDirectionRadians;
        settings.cascades[i].windSpeed = AppConfig::Water::kDefaultCascadeWindSpeeds[i];
        settings.cascades[i].fetchMeters = AppConfig::Water::kDefaultCascadeFetchMeters[i];
        settings.cascades[i].spreadBlend = AppConfig::Water::kDefaultCascadeSpreadBlend[i];
        settings.cascades[i].swell = AppConfig::Water::kDefaultCascadeSwell[i];
        settings.cascades[i].peakEnhancement = AppConfig::Water::kDefaultCascadePeakEnhancement[i];
        settings.cascades[i].shortWavesFade = AppConfig::Water::kDefaultCascadeShortWavesFade[i];
        settings.cascades[i].choppiness = AppConfig::Water::kDefaultCascadeChoppiness[i];
        settings.cascades[i].shallowDampingStrength = AppConfig::Water::kDefaultCascadeShallowDampingStrength[i];
        settings.cascades[i].updateModulo = AppConfig::Water::kDefaultCascadeUpdateModulo[i];
    }

    return settings;
}
