#pragma once

#include <SDL3/SDL_gpu.h>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>

namespace AppConfig
{
namespace Perf
{
inline constexpr double kHistorySeconds = 3.0;
inline constexpr float kGraphHeight = 180.0f;
inline constexpr float kFlameGraphRowHeight = 24.0f;
inline constexpr float kFlameGraphMinHeight = 120.0f;
} // namespace Perf

namespace Camera
{
inline constexpr float kNearPlane = 0.01f;
inline constexpr float kVerticalFovRadians = 1.05f;
inline constexpr glm::dvec3 kWorldUp{0.0, 1.0, 0.0};
inline constexpr glm::dvec3 kFallbackForward{0.0, 0.0, -1.0};
inline constexpr double kMoveSpeed = 2.6;
inline constexpr double kLookSpeedRadians = 1.8;
inline constexpr double kRollSpeedRadians = 1.6;
inline constexpr double kAltitudeSpeedScale = 0.1;
// Tokyo (139.6917 E, 35.6895 N) in the fixed Airocean atlas projection.
inline constexpr double kStartWorldX = 12376500.0;
inline constexpr double kStartWorldZ = -177296.0;
inline constexpr double kStartAltitude = 300.0;
} // namespace Camera

namespace Input
{
inline constexpr float kAxisDeadzone = 0.18f;
inline constexpr float kTriggerDeadzone = 0.08f;
} // namespace Input

namespace Quadtree
{
inline constexpr std::int64_t kNeighborRadius = 1;
inline constexpr double kMinimumQuadSize = 256.0;
inline constexpr double kSubdivisionDistanceFactor = 1.1;
inline constexpr double kDebugDepthHeightOffset = 0.05;
inline constexpr double kDebugBaseHeightOffset = 0.02;
} // namespace Quadtree

namespace Terrain
{
inline constexpr std::uint32_t kHeightmapResolution = 259;
inline constexpr std::uint32_t kHeightmapLeafResolution = kHeightmapResolution - 2;
inline constexpr std::uint32_t kHeightmapLeafIntervalCount = kHeightmapLeafResolution - 1;
inline constexpr std::uint32_t kHeightmapLeafHalo = 1;
inline constexpr std::uint32_t kRenderedPatchInset = 1;
inline constexpr std::uint32_t kHeightmapSliceCapacity = 512;
inline constexpr std::uint32_t kCpuHeightmapCacheCapacity = 16;
inline constexpr std::uint32_t kHeightmapReadbackCapacity = 8;
static_assert(kHeightmapReadbackCapacity > 0 && kHeightmapReadbackCapacity <= kCpuHeightmapCacheCapacity &&
              kCpuHeightmapCacheCapacity <= kHeightmapSliceCapacity);
inline constexpr std::uint32_t kSourceHeightmapTileResolution = 256;
inline constexpr std::uint32_t kSourceHeightmapTileIntervalCount = 255;
inline constexpr std::uint32_t kSourceHeightmapCacheCapacity = 64;
inline constexpr std::uint32_t kSourceHeightmapHashBucketCount = 64;
inline constexpr std::uint32_t kSourceHeightmapHashLookupDepth = 4;
inline constexpr std::uint32_t kMaxFinalHeightmapsPerDispatch = 16;
inline constexpr std::uint32_t kSourceHeightmapDescriptorCapacity = kSourceHeightmapCacheCapacity * kMaxFinalHeightmapsPerDispatch;
inline constexpr float kAmbientLight = 0.26f;
inline constexpr float kSolarHorizonFadeDegrees = 1.0f;
inline constexpr float kAmbientNightElevationDegrees = -6.0f;
inline constexpr float kAmbientDayElevationDegrees = 6.0f;
inline constexpr int kCloudShadowSamples = 8;
} // namespace Terrain

namespace Water
{
// Homogeneous clear-water participating-medium coefficients, in inverse meters.
inline constexpr float kMediumSourceScale = 4.8f;
inline constexpr glm::vec3 kMediumAbsorption{0.15f, 0.045f, 0.015f};
inline constexpr glm::vec3 kMediumScattering{0.006f, 0.012f, 0.018f};
inline constexpr bool kEnabled = true;
inline constexpr std::uint32_t kCascadeResolution = 512;
inline constexpr std::uint32_t kMaxCascadeCount = 4;
inline constexpr std::uint32_t kDefaultCascadeCount = 4;
inline constexpr std::array<std::uint32_t, kDefaultCascadeCount> kDefaultCascadeSizesMeters{
    125,
    600,
    1250,
    1750,
};

inline constexpr float kDefaultWaterLevel = 0.0f;
inline constexpr float kDefaultGlobalAmplitude = 1.1f;
inline constexpr float kDefaultGlobalChoppiness = 1.2f;
inline constexpr float kDefaultDepthMeters = 20.0f;
inline constexpr float kDefaultLowCutoff = 0.0001f;
inline constexpr float kDefaultHighCutoff = 9000.0f;

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeWindDirectionsRadians{
    4.0f,
    3.5f,
    3.0f,
    4.0f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeAmplitudes{
    0.25f,
    0.05f,
    0.05f,
    0.35f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeWindSpeeds{
    2.3f,
    12.0f,
    5.0f,
    8.0f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeFetchMeters{
    100000.0f,
    10000.0f,
    100000.0f,
    1000000.0f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeSpreadBlend{
    0.642f,
    0.96f,
    0.70f,
    0.82f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeSwell{
    1.0f,
    1.0f,
    1.0f,
    1.0f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadePeakEnhancement{
    1.7f,
    2.1f,
    2.6f,
    1.3f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeShortWavesFade{
    0.025f,
    0.01f,
    0.5f,
    0.5f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeChoppiness{
    3.30f,
    1.30f,
    1.90f,
    0.70f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeShallowDampingStrength{
    0.6f,
    0.7f,
    0.8f,
    1.0f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeShallowDampingDepthMeters{
    6.0f,
    12.0f,
    18.0f,
    24.0f,
};

inline constexpr std::array<std::uint32_t, kDefaultCascadeCount> kDefaultCascadeUpdateModulo{
    1u,
    1u,
    2u,
    4u,
};

inline constexpr float kExpectedWaveHeight = 8.0f;
inline constexpr float kVisibilityHeightPadding = 2.0f;
inline constexpr float kMaxTerrainMinHeightAboveWaterToDraw = 50.0f;
inline constexpr float kShallowDepthFadeStartMeters = 12.0f;
inline constexpr float kShallowDepthFadeEndMeters = 2.0f;
inline constexpr float kShorelineTintDepthMeters = 6.0f;
inline constexpr glm::vec3 kShallowWaterColor{0.30f, 0.66f, 0.70f};
inline constexpr glm::vec3 kMidWaterColor{0.11f, 0.40f, 0.52f};
inline constexpr glm::vec3 kDeepWaterColor{0.012f, 0.060f, 0.115f};
inline constexpr float kMidWaterDepthStartMeters = 3.0f;
inline constexpr float kMidWaterDepthEndMeters = 18.0f;
inline constexpr float kDeepWaterDepthStartMeters = 16.0f;
inline constexpr float kDeepWaterDepthEndMeters = 42.0f;
inline constexpr float kBaseRoughness = 0.08f;
inline constexpr float kSlopeRoughnessStrength = 0.18f;
inline constexpr float kEnvironmentReflectionStrength = 1.0f;
inline constexpr float kSubsurfaceStrength = 0.28f;
inline constexpr float kScatteringAnisotropy = 0.55f;
inline constexpr float kDepthAbsorptionStrength = 0.065f;
inline constexpr float kShallowRefractionMaxDepthMeters = 2.50f;
inline constexpr float kShallowRefractionFullFadeDepthMeters = 7.50f;
inline constexpr float kCascadeDetailTexelFadeStart = 0.35f;
inline constexpr float kCascadeDetailTexelFadeEnd = 1.35f;
inline constexpr float kFarNormalFadeStartMeters = 2000.0f;
inline constexpr float kFarNormalFadeEndMeters = 7000.0f;
inline constexpr float kFarRoughnessFadeStartMeters = 1200.0f;
inline constexpr float kFarRoughnessFadeEndMeters = 6000.0f;
inline constexpr float kFarRoughnessBoost = 0.22f;
inline constexpr float kFoamRoughness = 0.58f;
inline constexpr float kFarReflectionFlattenStartMeters = 3000.0f;
inline constexpr float kFarReflectionFlattenEndMeters = 10000.0f;
inline constexpr float kFarFoamFadeStartMeters = 1800.0f;
inline constexpr float kFarFoamFadeEndMeters = 5000.0f;
inline constexpr float kFoamCascadeDetailTexelThreshold = 0.55f;
inline constexpr bool kDefaultCrestFoamEnabled = true;
inline constexpr bool kDefaultDrawFoam = true;
inline constexpr bool kDefaultDrawTerrainCaustics = true;
inline constexpr float kDefaultCausticsIntensity = 0.210f;
inline constexpr float kDefaultCausticsPatternScaleA = 0.060f;
inline constexpr float kDefaultCausticsPatternScaleB = 0.070f;
inline constexpr float kDefaultCausticsRotationA = -0.57f;
inline constexpr float kDefaultCausticsRotationB = 0.91f;
inline constexpr float kDefaultCausticsDisplacementWarp = 0.12f;
inline constexpr float kDefaultCausticsSlopeWarp = 10.0f;
inline constexpr float kDefaultCausticsRidgeMinA = -0.090f;
inline constexpr float kDefaultCausticsRidgeMaxA = 0.000f;
inline constexpr float kDefaultCausticsRidgeMinB = -0.108f;
inline constexpr float kDefaultCausticsRidgeMaxB = 0.003f;
inline constexpr float kDefaultCausticsFocusMin = 0.08f;
inline constexpr float kDefaultCausticsFocusMax = 0.42f;
inline constexpr float kDefaultCausticsMinSurfaceUp = 0.45f;
inline constexpr float kDefaultCrestFoamAmount = 1.6f;
inline constexpr float kDefaultCrestFoamThreshold = 0.12f;
inline constexpr float kDefaultCrestFoamSoftness = 0.18f;
inline constexpr float kDefaultCrestFoamSlopeStart = 0.39f;
inline constexpr float kDefaultCrestFoamDecayRate = 0.015f;
inline constexpr float kDefaultCrestFoamBrightness = 1.35f;
inline constexpr float kDefaultFoamSdfSampleScaleA = 1.25f;
inline constexpr float kDefaultFoamSdfRidgeMinA = -0.015f;
inline constexpr float kDefaultFoamSdfRidgeMaxA = 0.000f;
inline constexpr float kDefaultFoamSdfRidgeMinB = 0.018f;
inline constexpr float kDefaultFoamSdfRidgeMaxB = 0.155f;
inline constexpr float kDefaultFoamNoiseScale = 0.130f;
inline constexpr float kDefaultFoamHistoryWarpStrength = 6.0f;
inline constexpr float kDefaultFoamDetailOffsetStrength = 0.052f;
inline constexpr float kDefaultFoamDetailBreakupStrength = 0.7f;
inline constexpr float kDefaultFoamDetailBreakupScale = 0.035f;
inline constexpr float kDefaultFoamEvolutionStart = 0.1f;
inline constexpr float kDefaultFoamEvolutionEnd = 1.0f;
inline constexpr float kDefaultFoamEvolutionDropoffEnd = 1.050f;
inline constexpr float kDefaultFoamFadeStart = 0.005f;
inline constexpr float kDefaultFoamFadeEnd = 0.070f;
inline constexpr bool kDefaultShoreFoamEnabled = true;
inline constexpr float kDefaultShoreFoamAmount = 0.65f;
inline constexpr float kDefaultShoreFoamDepthStart = 0.15f;
inline constexpr float kDefaultShoreFoamDepthEnd = 0.95f;
inline constexpr float kDefaultShoreFoamBreakupStrength = 0.40f;
inline constexpr float kDefaultShoreFoamDecayDepthStart = -0.25f;
inline constexpr float kDefaultShoreFoamDecayDepthEnd = 0.80f;
inline constexpr glm::vec3 kCrestFoamColor{0.92f, 0.97f, 1.0f};

inline constexpr std::uint32_t kMeshVertexResolution = 129;
inline constexpr std::uint32_t kMaxWaterInstances = 4096;
} // namespace Water

namespace Renderer
{
inline constexpr SDL_GPUSwapchainComposition kSwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
inline constexpr SDL_GPUPresentMode kPresentMode = SDL_GPU_PRESENTMODE_VSYNC;
} // namespace Renderer

namespace Foliage
{
inline constexpr bool kEnabled = true;
inline constexpr std::uint32_t kMarkerPageDrawCapacity = 1024u;
inline constexpr std::uint32_t kNearbyMarkerInstanceCapacity = 1024u;
inline constexpr bool kCanopyEnabled = true;
inline constexpr std::uint32_t kCanopyDrawCapacity = 256u;
inline constexpr float kCanopyShellHeightOffsetMeters = 10.0f;
} // namespace Foliage

namespace Light
{
// Orbit axis azimuth around world up used to define the sun's daily path.
inline constexpr float kSunAzimuthDegrees = 40.0f;
// Orbit axis elevation above the horizon used to tilt the sun's daily path.
inline constexpr float kSunElevationDegrees = 8.0f;
// Initial hour shown at startup.
inline constexpr float kTimeOfDayHours = 15.0f;
// Real seconds required for one in-game day at timeFactor 1.
inline constexpr float kDayLengthSeconds = 120000.0f;
// Multiplier on day/night progression speed.
inline constexpr float kTimeFactor = 1.0f;
// Direct-light tint applied to terrain and atmospheric single scattering.
inline constexpr glm::vec3 kSunColor{1.0f, 0.97f, 0.92f};
// Direct-light intensity multiplier.
inline constexpr float kSunIntensity = 1.35f;
} // namespace Light

namespace Atmosphere
{
// Approximate top of the participating atmosphere above sea level.
inline constexpr float kHeight = 85000.0f;
// Finite background-ray distance inside the medium (geometry uses reconstructed depth).
inline constexpr float kDistanceRange = 4000000.0f;

// Molecular scattering coefficients per channel. These drive blue-sky color.
inline constexpr float kRayleighScatterR = 7.400e-6f;
inline constexpr float kRayleighScatterG = 17.800e-6f;
inline constexpr float kRayleighScatterB = 45.500e-6f;

// Aerosol scattering and extinction. These mostly shape haze and the solar
// halo.
inline constexpr float kMieScatter = 1.700e-6f;
inline constexpr float kMieExtinction = 4.200e-6f;

// Coarse ozone absorption coefficients. Mostly affects sunset/sunrise
// coloration.
inline constexpr float kOzoneAbsorptionR = 0.650e-6f;
inline constexpr float kOzoneAbsorptionG = 1.881e-6f;
inline constexpr float kOzoneAbsorptionB = 0.085e-6f;

// Exponential falloff heights for each density layer.
inline constexpr float kRayleighScaleHeight = 8000.0f;
inline constexpr float kMieScaleHeight = 1200.0f;
inline constexpr float kOzoneColumnHeight = 25000.0f;

// Henyey-Greenstein anisotropy for aerosols. Higher values tighten the sun
// halo.
inline constexpr float kMieG = 0.88f;

// Source scale on atmospheric scattering and cloud incident sunlight.
inline constexpr float kAtmosphereCloudSourceScale = 4.8f;
// Shared atmosphere/cloud radiance calibration; decoded space is a separate source.
inline constexpr float kEnvironmentRadianceScale = 12.0f;
inline constexpr float kSpaceRadiance = 0.02f;
// Independent disk calibration against artistic solar RGB, not solid-angle irradiance.
inline constexpr float kSunDiskRadiance = 100.0f;
} // namespace Atmosphere
} // namespace AppConfig
