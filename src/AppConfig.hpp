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
inline constexpr double kNoiseFrequency = 1.0 / 1800.0;
inline constexpr float kHeightAmplitude = 1000.0f;
inline constexpr float kBaseHeight = 0.0f;
inline constexpr float kAmbientLight = 0.26f;
}

namespace Renderer
{
inline constexpr SDL_GPUSwapchainComposition kSwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
inline constexpr SDL_GPUPresentMode kPresentMode = SDL_GPU_PRESENTMODE_VSYNC;
}
}
