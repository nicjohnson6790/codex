#pragma once

#include "AppConfig.hpp"
#include "Position.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <array>
#include <cstdint>

enum class WaterMeshLod : std::uint8_t
{
    Lod0 = 0,
    Lod1 = 1,
    Lod2 = 2,
    Lod3 = 3,
    Lod4 = 4,
};

struct WaterMeshLodSettings
{
    std::uint32_t vertexResolution = 65;
    float maxDistanceMeters = 1000.0f;
    bool enabled = true;
    std::uint8_t reserved0 = 0;
    std::uint8_t reserved1 = 0;
    std::uint8_t reserved2 = 0;
};

struct WaterCascadeSettings
{
    float worldSizeMeters = 256.0f;
    float amplitude = 1.0f;
    float windDirectionRadians = 0.0f;
    float windSpeed = 18.0f;
    float choppiness = 1.0f;
    std::uint32_t updateModulo = 1;
};

struct WaterLodPolicySettings
{
    std::array<std::uint8_t, 8> meshLodForQuadtreeHint{
        0, 1, 2, 3, 4, 4, 4, 4,
    };
    bool useDistanceOverride = true;
    bool useLeafSizeOverride = true;
    std::array<float, AppConfig::Water::kMeshLodCount> maxLeafSizeForMeshLod{
        256.0f,
        1024.0f,
        4096.0f,
        16384.0f,
        3.402823466e+38f,
    };
    std::array<float, AppConfig::Water::kMeshLodCount> maxDistanceForMeshLod{
        300.0f,
        1200.0f,
        5000.0f,
        20000.0f,
        3.402823466e+38f,
    };
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
    std::array<WaterMeshLodSettings, AppConfig::Water::kMeshLodCount> meshLods{};
    WaterLodPolicySettings lodPolicy{};
};

struct WaterLeafDrawRequest
{
    WorldGridQuadtreeLeafId leafId{};
    Position origin{};
    double sizeMeters = 0.0;
    std::uint8_t quadtreeLodHint = 0;
    std::uint8_t waterMeshLod = 0;
    std::uint32_t bandMask = 0;
};
