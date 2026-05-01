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
}

namespace Camera
{
inline constexpr float kNearPlane = 0.01f;
inline constexpr float kVerticalFovRadians = 1.05f;
inline constexpr glm::dvec3 kWorldUp{ 0.0, 1.0, 0.0 };
inline constexpr glm::dvec3 kFallbackForward{ 0.0, 0.0, -1.0 };
inline constexpr double kMoveSpeed = 2.6;
inline constexpr double kLookSpeedRadians = 1.8;
inline constexpr double kRollSpeedRadians = 1.6;
inline constexpr double kAltitudeSpeedScale = 0.1;
}

namespace Input
{
inline constexpr float kAxisDeadzone = 0.18f;
inline constexpr float kTriggerDeadzone = 0.08f;
}

namespace Quadtree
{
inline constexpr std::int64_t kNeighborRadius = 1;
inline constexpr double kMinimumQuadSize = 256.0;
inline constexpr double kSubdivisionDistanceFactor = 1.1;
inline constexpr double kDebugDepthHeightOffset = 0.05;
inline constexpr double kDebugBaseHeightOffset = 0.02;
}

namespace Terrain
{
inline constexpr std::uint32_t kHeightmapResolution = 259;
inline constexpr std::uint32_t kHeightmapLeafResolution = kHeightmapResolution - 2;
inline constexpr std::uint32_t kHeightmapLeafIntervalCount = kHeightmapLeafResolution - 1;
inline constexpr std::uint32_t kHeightmapLeafHalo = 1;
inline constexpr std::uint32_t kRenderedPatchInset = 1;
inline constexpr std::uint32_t kHeightmapSliceCapacity = 512;
inline constexpr float kBaseHeight = 0.0f;
inline constexpr float kAmbientLight = 0.26f;

inline constexpr double kHillsWavelength = 2000.0;
inline constexpr double kHillsAmplitude = 400.0;
inline constexpr double kHillsBias = 200.0;
inline constexpr double kHillsInitialFrequency = 0.75;
inline constexpr double kHillsInitialAmplitude = 1.0;
inline constexpr std::uint32_t kHillsOctaveCount = 3;
inline constexpr double kHillsOctaveFrequencyScale = 1.8;
inline constexpr double kHillsOctaveAmplitudeScale = 0.45;
inline constexpr double kHillsGradientDampenStrength = 0.00;
inline constexpr double kHillsOctaveRotationDegrees = 30.0;

inline constexpr double kMediumDetailWavelength = 6000.0;
inline constexpr double kMediumDetailAmplitude = 800.0;
inline constexpr double kMediumDetailBias = 700.0;
inline constexpr double kMediumDetailInitialFrequency = 1.0;
inline constexpr double kMediumDetailInitialAmplitude = 1.0;
inline constexpr std::uint32_t kMediumDetailOctaveCount = 6;
inline constexpr double kMediumDetailOctaveFrequencyScale = 2.0;
inline constexpr double kMediumDetailOctaveAmplitudeScale = 0.5;
inline constexpr double kMediumDetailGradientDampenStrength = 0.40;
inline constexpr double kMediumDetailOctaveRotationDegrees = 30.0;

inline constexpr double kHighDetailWavelength = 2000.0;
inline constexpr double kHighDetailAmplitude = 1600.0;
inline constexpr double kHighDetailBias = 1500.0;
inline constexpr double kHighDetailInitialFrequency = 1.0;
inline constexpr double kHighDetailInitialAmplitude = 1.0;
inline constexpr std::uint32_t kHighDetailOctaveCount = 8;
inline constexpr double kHighDetailOctaveFrequencyScale = 2.0;
inline constexpr double kHighDetailOctaveAmplitudeScale = 0.5;
inline constexpr double kHighDetailGradientDampenStrength = 0.85;
inline constexpr double kHighDetailOctaveRotationDegrees = 30.0;

inline constexpr double kBlendWavelength = 30000.0;
inline constexpr double kBlendInitialFrequency = 1.0;
inline constexpr double kBlendInitialAmplitude = 1.0;
inline constexpr std::uint32_t kBlendOctaveCount = 5;
inline constexpr double kBlendOctaveFrequencyScale = 2.0;
inline constexpr double kBlendOctaveAmplitudeScale = 0.5;
inline constexpr double kBlendGradientDampenStrength = 0.0;
inline constexpr double kBlendOctaveRotationDegrees = 30.0;
inline constexpr double kBlendLowThreshold = 1.00;
inline constexpr double kBlendHighThreshold = 1.15;
inline constexpr double kBlendLowTransitionWidth = 0.15;
inline constexpr double kBlendHighTransitionWidth = 0.13;
}

namespace Water
{
inline constexpr bool kEnabled = true;
inline constexpr std::uint32_t kCascadeResolution = 512;
inline constexpr std::uint32_t kMaxCascadeCount = 4;
inline constexpr std::uint32_t kDefaultCascadeCount = 4;
inline constexpr std::array<std::uint32_t, kDefaultCascadeCount> kDefaultCascadeSizesMeters{
    125,
    500,
    2000,
    8000,
};

inline constexpr float kDefaultWaterLevel = 250.0f;
inline constexpr float kDefaultGlobalAmplitude = 1.1f;
inline constexpr float kDefaultGlobalChoppiness = 1.2f;
inline constexpr float kDefaultDepthMeters = 20.0f;
inline constexpr float kDefaultLowCutoff = 0.0001f;
inline constexpr float kDefaultHighCutoff = 9000.0f;

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeWindDirectionsRadians{
    1.0f,
    2.0f,
    3.0f,
    4.0f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeAmplitudes{
    0.55f,
    2.20f,
    0.20f,
    0.25f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeWindSpeeds{
    0.80f,
    7.0f,
    1.00f,
    10.00f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeFetchMeters{
    100000.0f,
    1000.0f,
    100000000.0f,
    1000000.0f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeSpreadBlend{
    0.642f,
    0.0f,
    0.14f,
    0.47f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeSwell{
    1.0f,
    1.0f,
    1.0f,
    1.0f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadePeakEnhancement{
    1.0f,
    1.0f,
    2.6f,
    1.0f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeShortWavesFade{
    0.025f,
    0.01f,
    0.5f,
    0.5f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeChoppiness{
    3.30f,
    7.25f,
    2.85f,
    1.0f,
};

inline constexpr std::array<float, kDefaultCascadeCount> kDefaultCascadeShallowDampingStrength{
    0.6f,
    0.7f,
    0.8f,
    1.0f,
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
inline constexpr float kBaseReflectance = 0.02037f;
inline constexpr float kBaseRoughness = 0.08f;
inline constexpr float kSlopeRoughnessStrength = 0.18f;
inline constexpr float kEnvironmentReflectionStrength = 1.0f;
inline constexpr float kSubsurfaceStrength = 0.28f;
inline constexpr float kScatteringAnisotropy = 0.55f;
inline constexpr float kDepthAbsorptionStrength = 0.065f;
inline constexpr float kShallowRefractionMaxDepthMeters = 2.50f;
inline constexpr float kShallowRefractionFullFadeDepthMeters = 7.50f;
inline constexpr bool kDefaultCrestFoamEnabled = true;
inline constexpr float kDefaultCrestFoamAmount = 1.85f;
inline constexpr float kDefaultCrestFoamThreshold = 0.12f;
inline constexpr float kDefaultCrestFoamSoftness = 0.18f;
inline constexpr float kDefaultCrestFoamSlopeStart = 0.21f;
inline constexpr float kDefaultCrestFoamDecayRate = 0.05f;
inline constexpr float kDefaultCrestFoamBrightness = 1.35f;
inline constexpr glm::vec3 kCrestFoamColor{ 0.92f, 0.97f, 1.0f };

inline constexpr std::uint32_t kMeshVertexResolution = 129;
inline constexpr std::uint32_t kMaxWaterInstances = 4096;
}

namespace Renderer
{
inline constexpr SDL_GPUSwapchainComposition kSwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
inline constexpr SDL_GPUPresentMode kPresentMode = SDL_GPU_PRESENTMODE_VSYNC;
}

namespace Light
{
// Orbit axis azimuth around world up used to define the sun's daily path.
inline constexpr float kSunAzimuthDegrees = 40.0f;
// Orbit axis elevation above the horizon used to tilt the sun's daily path.
inline constexpr float kSunElevationDegrees = 8.0f;
// Initial hour shown at startup.
inline constexpr float kTimeOfDayHours = 10.0f;
// Real seconds required for one in-game day at timeFactor 1.
inline constexpr float kDayLengthSeconds = 120.0f;
// Multiplier on day/night progression speed.
inline constexpr float kTimeFactor = 1.0f;
// Direct-light tint applied to terrain and the atmosphere LUT sun term.
inline constexpr glm::vec3 kSunColor{ 1.0f, 0.97f, 0.92f };
// Direct-light intensity multiplier.
inline constexpr float kSunIntensity = 1.35f;
}

namespace Atmosphere
{
// Approximate top of the participating atmosphere above sea level.
inline constexpr float kHeight = 85000.0f;
// Largest path length encoded in the LUT's logarithmic distance axis.
inline constexpr float kDistanceRange = 4000000.0f;

// Molecular scattering coefficients per channel. These drive blue-sky color.
inline constexpr float kRayleighScatterR = 7.400e-6f;
inline constexpr float kRayleighScatterG = 17.800e-6f;
inline constexpr float kRayleighScatterB = 45.500e-6f;

// Aerosol scattering and extinction. These mostly shape haze and the solar halo.
inline constexpr float kMieScatter = 1.700e-6f;
inline constexpr float kMieExtinction = 4.200e-6f;

// Coarse ozone absorption coefficients. Mostly affects sunset/sunrise coloration.
inline constexpr float kOzoneAbsorptionR = 0.650e-6f;
inline constexpr float kOzoneAbsorptionG = 1.881e-6f;
inline constexpr float kOzoneAbsorptionB = 0.085e-6f;

// Exponential falloff heights for each density layer.
inline constexpr float kRayleighScaleHeight = 8000.0f;
inline constexpr float kMieScaleHeight = 1200.0f;
inline constexpr float kOzoneColumnHeight = 25000.0f;

// Henyey-Greenstein anisotropy for aerosols. Higher values tighten the sun halo.
inline constexpr float kMieG = 0.88f;

// Post-exposure applied to the LUT's in-scattered radiance before encoding.
inline constexpr float kExposure = 4.8f;
// Extra opacity derived from view-path transmittance.
inline constexpr float kAlphaScale = 1.8f;

// Broad non-solar sky fill. This is more artistic than strictly physical.
inline constexpr float kAmbientSkyScale = 0.30f;
// Blend between a hand-shaped blue dome tint and normalized Rayleigh tint.
inline constexpr float kAmbientBlueBias = 0.65f;
// How much direct solar color is allowed to bleed into the broad sky fill.
inline constexpr float kAmbientSolarInfluence = 0.12f;
// Extra solar-color influence during twilight.
inline constexpr float kAmbientTwilightInfluence = 0.18f;
// Base blue tint used to keep the dome visibly blue during the day.
inline constexpr glm::vec3 kAmbientBlueTint{ 0.18f, 0.34f, 1.10f };
// Multiplier on the normalized Rayleigh tint contribution.
inline constexpr float kRayleighTintScale = 1.85f;

// Additional far-distance veil color and strength. Also artistic shaping terms.
inline constexpr glm::vec3 kHazeColor{ 0.52f, 0.66f, 0.90f };
inline constexpr float kHazeStrength = 0.25f;
inline constexpr float kPathFogDistance = 360000.0f;
inline constexpr float kLongRangeHazeDistance = 480000.0f;

// Sunward halo shaping. Higher powers tighten the visible lobe.
inline constexpr float kAureolePower = 96.0f;
inline constexpr float kAureoleStrength = 0.60f;
inline constexpr float kSunDiskPower = 1840.0f;
inline constexpr float kSunDiskStrength = 8.5f;
inline constexpr float kSunGlowPower = 18.0f;

// Warm low-sun scattering shaping used for sunrise and sunset color.
inline constexpr glm::vec3 kSunsetTint{ 1.10f, 0.42f, 0.12f };
inline constexpr float kSunsetStrength = 2.22f;
inline constexpr float kSunsetSunwardBoost = 1.32f;
inline constexpr float kSunsetDistanceMin = 0.30f;
inline constexpr float kSunsetDistanceMax = 0.70f;
}
}
