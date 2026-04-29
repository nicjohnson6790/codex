#pragma once

#include "AppConfig.hpp"
#include "WaterTypes.hpp"

inline WaterSettings makeDefaultWaterSettings()
{
    WaterSettings settings{};
    constexpr float kDefaultCascadeAmplitudes[AppConfig::Water::kDefaultCascadeCount]{
        0.55f,
        1.10f,
        1.45f,
        1.15f,
        0.65f,
        0.30f,
    };

    settings.enabled = AppConfig::Water::kEnabled;
    settings.showLodTint = true;
    settings.waterLevel = AppConfig::Water::kDefaultWaterLevel;
    settings.globalAmplitude = AppConfig::Water::kDefaultAmplitude;
    settings.globalChoppiness = AppConfig::Water::kDefaultChoppiness;
    settings.cascadeCount = AppConfig::Water::kDefaultCascadeCount;

    for (std::uint32_t i = 0; i < settings.cascadeCount; ++i)
    {
        settings.cascades[i].worldSizeMeters = static_cast<float>(AppConfig::Water::kDefaultCascadeSizesMeters[i]);
        settings.cascades[i].amplitude = kDefaultCascadeAmplitudes[i];
        settings.cascades[i].windDirectionRadians = AppConfig::Water::kDefaultWindDirectionRadians;
        settings.cascades[i].windSpeed = AppConfig::Water::kDefaultWindSpeed;
        settings.cascades[i].choppiness = AppConfig::Water::kDefaultChoppiness;
        settings.cascades[i].updateModulo = 1;
    }

    return settings;
}
