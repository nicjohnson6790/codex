#pragma once

#include "AppConfig.hpp"
#include "WaterTypes.hpp"

inline WaterSettings makeDefaultWaterSettings()
{
    WaterSettings settings{};

    settings.enabled = AppConfig::Water::kEnabled;
    settings.showLodTint = true;
    settings.waterLevel = AppConfig::Water::kDefaultWaterLevel;
    settings.globalAmplitude = AppConfig::Water::kDefaultAmplitude;
    settings.globalChoppiness = AppConfig::Water::kDefaultChoppiness;
    settings.cascadeCount = AppConfig::Water::kDefaultCascadeCount;

    for (std::uint32_t i = 0; i < settings.cascadeCount; ++i)
    {
        settings.cascades[i].worldSizeMeters = static_cast<float>(AppConfig::Water::kDefaultCascadeSizesMeters[i]);
        settings.cascades[i].amplitude = 1.0f;
        settings.cascades[i].windDirectionRadians = AppConfig::Water::kDefaultWindDirectionRadians;
        settings.cascades[i].windSpeed = AppConfig::Water::kDefaultWindSpeed;
        settings.cascades[i].choppiness = AppConfig::Water::kDefaultChoppiness;
        settings.cascades[i].updateModulo = 1;
    }

    return settings;
}
