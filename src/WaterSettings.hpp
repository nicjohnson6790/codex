#pragma once

#include "AppConfig.hpp"
#include "WaterTypes.hpp"

inline WaterSettings makeDefaultWaterSettings()
{
    WaterSettings settings{};
    settings.enabled = AppConfig::Water::kEnabled;
    settings.showLodTint = false;
    settings.waterLevel = AppConfig::Water::kDefaultWaterLevel;
    settings.globalAmplitude = AppConfig::Water::kDefaultGlobalAmplitude;
    settings.globalChoppiness = AppConfig::Water::kDefaultGlobalChoppiness;
    settings.depthMeters = AppConfig::Water::kDefaultDepthMeters;
    settings.lowCutoff = AppConfig::Water::kDefaultLowCutoff;
    settings.highCutoff = AppConfig::Water::kDefaultHighCutoff;
    settings.crestFoamEnabled = AppConfig::Water::kDefaultCrestFoamEnabled;
    settings.drawFoam = AppConfig::Water::kDefaultDrawFoam;
    settings.drawTerrainCaustics = AppConfig::Water::kDefaultDrawTerrainCaustics;
    settings.crestFoamAmount = AppConfig::Water::kDefaultCrestFoamAmount;
    settings.crestFoamThreshold = AppConfig::Water::kDefaultCrestFoamThreshold;
    settings.crestFoamSoftness = AppConfig::Water::kDefaultCrestFoamSoftness;
    settings.crestFoamSlopeStart = AppConfig::Water::kDefaultCrestFoamSlopeStart;
    settings.crestFoamDecayRate = AppConfig::Water::kDefaultCrestFoamDecayRate;
    settings.crestFoamBrightness = AppConfig::Water::kDefaultCrestFoamBrightness;
    settings.causticsIntensity = AppConfig::Water::kDefaultCausticsIntensity;
    settings.causticsPatternScaleA = AppConfig::Water::kDefaultCausticsPatternScaleA;
    settings.causticsPatternScaleB = AppConfig::Water::kDefaultCausticsPatternScaleB;
    settings.causticsRotationA = AppConfig::Water::kDefaultCausticsRotationA;
    settings.causticsRotationB = AppConfig::Water::kDefaultCausticsRotationB;
    settings.causticsDisplacementWarpStrength = AppConfig::Water::kDefaultCausticsDisplacementWarp;
    settings.causticsSlopeWarpStrength = AppConfig::Water::kDefaultCausticsSlopeWarp;
    settings.causticsRidgeMinA = AppConfig::Water::kDefaultCausticsRidgeMinA;
    settings.causticsRidgeMaxA = AppConfig::Water::kDefaultCausticsRidgeMaxA;
    settings.causticsRidgeMinB = AppConfig::Water::kDefaultCausticsRidgeMinB;
    settings.causticsRidgeMaxB = AppConfig::Water::kDefaultCausticsRidgeMaxB;
    settings.causticsFocusMin = AppConfig::Water::kDefaultCausticsFocusMin;
    settings.causticsFocusMax = AppConfig::Water::kDefaultCausticsFocusMax;
    settings.causticsMinSurfaceUp = AppConfig::Water::kDefaultCausticsMinSurfaceUp;
    settings.maxTerrainMinHeightAboveWaterToDraw = AppConfig::Water::kMaxTerrainMinHeightAboveWaterToDraw;
    settings.cascadeCount = AppConfig::Water::kDefaultCascadeCount;

    for (std::uint32_t i = 0; i < settings.cascadeCount; ++i)
    {
        settings.cascades[i].worldSizeMeters = static_cast<float>(AppConfig::Water::kDefaultCascadeSizesMeters[i]);
        settings.cascades[i].amplitude = AppConfig::Water::kDefaultCascadeAmplitudes[i];
        settings.cascades[i].windDirectionRadians = AppConfig::Water::kDefaultCascadeWindDirectionsRadians[i];
        settings.cascades[i].windSpeed = AppConfig::Water::kDefaultCascadeWindSpeeds[i];
        settings.cascades[i].fetchMeters = AppConfig::Water::kDefaultCascadeFetchMeters[i];
        settings.cascades[i].spreadBlend = AppConfig::Water::kDefaultCascadeSpreadBlend[i];
        settings.cascades[i].swell = AppConfig::Water::kDefaultCascadeSwell[i];
        settings.cascades[i].peakEnhancement = AppConfig::Water::kDefaultCascadePeakEnhancement[i];
        settings.cascades[i].shortWavesFade = AppConfig::Water::kDefaultCascadeShortWavesFade[i];
        settings.cascades[i].choppiness = AppConfig::Water::kDefaultCascadeChoppiness[i];
        settings.cascades[i].shallowDampingStrength = AppConfig::Water::kDefaultCascadeShallowDampingStrength[i];
        settings.cascades[i].foamDetailScale = AppConfig::Water::kDefaultCascadeFoamDetailScales[i];
        settings.cascades[i].updateModulo = AppConfig::Water::kDefaultCascadeUpdateModulo[i];
    }

    return settings;
}
