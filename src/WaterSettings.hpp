#pragma once

#include "AppConfig.hpp"
#include "WaterTypes.hpp"

inline WaterSettings makeDefaultWaterSettings()
{
    WaterSettings settings{};

    settings.enabled = AppConfig::Water::kEnabled;
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

    for (std::uint32_t i = 0; i < AppConfig::Water::kMeshLodCount; ++i)
    {
        settings.meshLods[i].vertexResolution = AppConfig::Water::kDefaultMeshLodVertexResolutions[i];
        settings.meshLods[i].enabled = true;
    }

    settings.meshLods[0].maxDistanceMeters = AppConfig::Water::kDefaultLod0MaxDistanceMeters;
    settings.meshLods[1].maxDistanceMeters = AppConfig::Water::kDefaultLod1MaxDistanceMeters;
    settings.meshLods[2].maxDistanceMeters = AppConfig::Water::kDefaultLod2MaxDistanceMeters;
    settings.meshLods[3].maxDistanceMeters = AppConfig::Water::kDefaultLod3MaxDistanceMeters;
    settings.meshLods[4].maxDistanceMeters = 3.402823466e+38f;

    settings.lodPolicy.meshLodForQuadtreeHint = {
        AppConfig::Water::kDefaultWaterMeshLodForQuadtreeHint0,
        AppConfig::Water::kDefaultWaterMeshLodForQuadtreeHint1,
        AppConfig::Water::kDefaultWaterMeshLodForQuadtreeHint2,
        AppConfig::Water::kDefaultWaterMeshLodForQuadtreeHint3,
        AppConfig::Water::kDefaultWaterMeshLodForQuadtreeHint4,
        AppConfig::Water::kDefaultWaterMeshLodForQuadtreeHint4,
        AppConfig::Water::kDefaultWaterMeshLodForQuadtreeHint4,
        AppConfig::Water::kDefaultWaterMeshLodForQuadtreeHint4,
    };

    settings.lodPolicy.useDistanceOverride = true;
    settings.lodPolicy.useLeafSizeOverride = true;
    settings.lodPolicy.maxLeafSizeForMeshLod = {
        256.0f,
        1024.0f,
        4096.0f,
        16384.0f,
        3.402823466e+38f,
    };
    settings.lodPolicy.maxDistanceForMeshLod = {
        AppConfig::Water::kDefaultLod0MaxDistanceMeters,
        AppConfig::Water::kDefaultLod1MaxDistanceMeters,
        AppConfig::Water::kDefaultLod2MaxDistanceMeters,
        AppConfig::Water::kDefaultLod3MaxDistanceMeters,
        3.402823466e+38f,
    };

    return settings;
}
