#pragma once

#include "AppConfig.hpp"
#include "Position.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <array>
#include <cstdint>

#include <glm/vec3.hpp>

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
    float shallowDampingDepthMeters = AppConfig::Water::kDefaultCascadeShallowDampingDepthMeters[0];
    std::uint32_t updateModulo = 1;
};

struct WaterSettings
{
    bool enabled = true;
    bool showLodTint = false;
    bool crestFoamEnabled = AppConfig::Water::kDefaultCrestFoamEnabled;
    bool drawFoam = AppConfig::Water::kDefaultDrawFoam;
    bool drawTerrainCaustics = AppConfig::Water::kDefaultDrawTerrainCaustics;
    bool shoreFoamEnabled = AppConfig::Water::kDefaultShoreFoamEnabled;
    glm::vec3 shallowWaterColor = AppConfig::Water::kShallowWaterColor;
    glm::vec3 midWaterColor = AppConfig::Water::kMidWaterColor;
    glm::vec3 deepWaterColor = AppConfig::Water::kDeepWaterColor;
    float waterLevel = AppConfig::Water::kDefaultWaterLevel;
    float globalAmplitude = AppConfig::Water::kDefaultGlobalAmplitude;
    float globalChoppiness = AppConfig::Water::kDefaultGlobalChoppiness;
    float depthMeters = AppConfig::Water::kDefaultDepthMeters;
    float midWaterDepthStart = AppConfig::Water::kMidWaterDepthStartMeters;
    float midWaterDepthEnd = AppConfig::Water::kMidWaterDepthEndMeters;
    float deepWaterDepthStart = AppConfig::Water::kDeepWaterDepthStartMeters;
    float deepWaterDepthEnd = AppConfig::Water::kDeepWaterDepthEndMeters;
    float lowCutoff = AppConfig::Water::kDefaultLowCutoff;
    float highCutoff = AppConfig::Water::kDefaultHighCutoff;
    float crestFoamAmount = AppConfig::Water::kDefaultCrestFoamAmount;
    float crestFoamThreshold = AppConfig::Water::kDefaultCrestFoamThreshold;
    float crestFoamSoftness = AppConfig::Water::kDefaultCrestFoamSoftness;
    float crestFoamSlopeStart = AppConfig::Water::kDefaultCrestFoamSlopeStart;
    float crestFoamDecayRate = AppConfig::Water::kDefaultCrestFoamDecayRate;
    float crestFoamBrightness = AppConfig::Water::kDefaultCrestFoamBrightness;
    float foamSdfSampleScaleA = AppConfig::Water::kDefaultFoamSdfSampleScaleA;
    float foamSdfRidgeMinA = AppConfig::Water::kDefaultFoamSdfRidgeMinA;
    float foamSdfRidgeMaxA = AppConfig::Water::kDefaultFoamSdfRidgeMaxA;
    float foamSdfRidgeMinB = AppConfig::Water::kDefaultFoamSdfRidgeMinB;
    float foamSdfRidgeMaxB = AppConfig::Water::kDefaultFoamSdfRidgeMaxB;
    float foamNoiseScale = AppConfig::Water::kDefaultFoamNoiseScale;
    float foamHistoryWarpStrength = AppConfig::Water::kDefaultFoamHistoryWarpStrength;
    float foamDetailOffsetStrength = AppConfig::Water::kDefaultFoamDetailOffsetStrength;
    float foamDetailBreakupStrength = AppConfig::Water::kDefaultFoamDetailBreakupStrength;
    float foamDetailBreakupScale = AppConfig::Water::kDefaultFoamDetailBreakupScale;
    float foamEvolutionStart = AppConfig::Water::kDefaultFoamEvolutionStart;
    float foamEvolutionEnd = AppConfig::Water::kDefaultFoamEvolutionEnd;
    float foamEvolutionDropoffEnd = AppConfig::Water::kDefaultFoamEvolutionDropoffEnd;
    float foamFadeStart = AppConfig::Water::kDefaultFoamFadeStart;
    float foamFadeEnd = AppConfig::Water::kDefaultFoamFadeEnd;
    float shoreFoamAmount = AppConfig::Water::kDefaultShoreFoamAmount;
    float shoreFoamDepthStart = AppConfig::Water::kDefaultShoreFoamDepthStart;
    float shoreFoamDepthEnd = AppConfig::Water::kDefaultShoreFoamDepthEnd;
    float shoreFoamBreakupStrength = AppConfig::Water::kDefaultShoreFoamBreakupStrength;
    float shoreFoamDecayDepthStart = AppConfig::Water::kDefaultShoreFoamDecayDepthStart;
    float shoreFoamDecayDepthEnd = AppConfig::Water::kDefaultShoreFoamDecayDepthEnd;
    float causticsIntensity = AppConfig::Water::kDefaultCausticsIntensity;
    float causticsPatternScaleA = AppConfig::Water::kDefaultCausticsPatternScaleA;
    float causticsPatternScaleB = AppConfig::Water::kDefaultCausticsPatternScaleB;
    float causticsRotationA = AppConfig::Water::kDefaultCausticsRotationA;
    float causticsRotationB = AppConfig::Water::kDefaultCausticsRotationB;
    float causticsDisplacementWarpStrength = AppConfig::Water::kDefaultCausticsDisplacementWarp;
    float causticsSlopeWarpStrength = AppConfig::Water::kDefaultCausticsSlopeWarp;
    float causticsRidgeMinA = AppConfig::Water::kDefaultCausticsRidgeMinA;
    float causticsRidgeMaxA = AppConfig::Water::kDefaultCausticsRidgeMaxA;
    float causticsRidgeMinB = AppConfig::Water::kDefaultCausticsRidgeMinB;
    float causticsRidgeMaxB = AppConfig::Water::kDefaultCausticsRidgeMaxB;
    float causticsFocusMin = AppConfig::Water::kDefaultCausticsFocusMin;
    float causticsFocusMax = AppConfig::Water::kDefaultCausticsFocusMax;
    float causticsMinSurfaceUp = AppConfig::Water::kDefaultCausticsMinSurfaceUp;
    float maxTerrainMinHeightAboveWaterToDraw = AppConfig::Water::kMaxTerrainMinHeightAboveWaterToDraw;
    std::uint32_t cascadeCount = AppConfig::Water::kDefaultCascadeCount;
    std::array<WaterCascadeSettings, AppConfig::Water::kMaxCascadeCount> cascades{};
};

struct WaterLeafDrawRequest
{
    enum class Type : std::uint8_t
    {
        Leaf = 0,
        Bridge = 1,
        CoarseBridge = 2,
    };

    Type type = Type::Leaf;
    WorldGridQuadtreeLeafId leafId{};
    Position origin{};
    double sizeMeters = 0.0;
    std::uint8_t quadtreeLodHint = 0;
    std::uint8_t edgeIndex = 0;
    std::uint32_t bandMask = 0;
    std::uint16_t terrainSliceIndex = 0;
    bool hasTerrainSlice = false;
};
