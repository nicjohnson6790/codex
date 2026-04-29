#pragma once

#include "AppConfig.hpp"
#include "Position.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <array>
#include <cstdint>

struct WaterCascadeSettings
{
    float worldSizeMeters = 256.0f;
    float amplitude = 1.0f;
    float windDirectionRadians = 0.0f;
    float windSpeed = 18.0f;
    float choppiness = 1.0f;
    std::uint32_t updateModulo = 1;
};

struct WaterSettings
{
    bool enabled = true;
    bool showLodTint = true;
    float waterLevel = 0.0f;
    float globalAmplitude = 1.0f;
    float globalChoppiness = 1.0f;
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
};
