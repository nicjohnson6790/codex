#pragma once

#include <SDL3/SDL_gpu.h>
#include <glm/vec3.hpp>

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

namespace Renderer
{
inline constexpr SDL_GPUSwapchainComposition kSwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
inline constexpr SDL_GPUPresentMode kPresentMode = SDL_GPU_PRESENTMODE_VSYNC;
}
}
