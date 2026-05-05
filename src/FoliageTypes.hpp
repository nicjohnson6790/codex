#pragma once

#include "Position.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <array>
#include <cstdint>

using FoliagePackedInstance = std::uint32_t;

namespace FoliageConfig
{
inline constexpr std::uint32_t kPageSizeMeters = 256u;
inline constexpr std::uint32_t kCandidateGridResolution = 64u;
inline constexpr std::uint32_t kCandidateSlotCount =
    kCandidateGridResolution * kCandidateGridResolution;
inline constexpr std::uint32_t kCandidateCellSizeMeters = kPageSizeMeters / kCandidateGridResolution;
inline constexpr std::uint16_t kPagePoolCapacity = 1024u;
inline constexpr std::uint16_t kLookupBucketCount = 256u;
inline constexpr std::uint16_t kLookupBucketEntryCount = 16u;
inline constexpr std::uint16_t kLookupOverflowCapacity = 1024u;
inline constexpr std::uint16_t kGenerationBudgetPerFrame = 4u;
inline constexpr float kBaseDensity = 0.08f;
inline constexpr float kClusterDensity = 0.18f;
}

struct FoliageResidentPageEntry
{
    WorldGridQuadtreeLeafId leafId{};
    std::uint16_t pageIndex = 0;
    std::uint16_t liveCount = 0;
    std::uint8_t age = 0;
    std::uint8_t flags = 0;
};

struct FoliageReadyPageInfo
{
    std::uint16_t pageIndex = 0;
    std::uint16_t liveCount = 0;
    std::uint32_t seed = 0;
};

struct FoliagePageDrawReference
{
    std::uint16_t pageIndex = 0;
    std::uint16_t liveCount = 0;
    std::uint32_t seed = 0;
    Position pageOrigin{};
    Position terrainLeafOrigin{};
    std::uint16_t terrainSliceIndex = 0;
    std::uint8_t terrainScalePow = 0;
};

struct FoliageTerrainSource
{
    WorldGridQuadtreeLeafId terrainLeafId{};
    std::uint16_t terrainSliceIndex = 0;
};

[[nodiscard]] inline FoliagePackedInstance packFoliageInstance(
    std::uint16_t candidateSlot,
    std::uint16_t meshId,
    std::uint8_t flags = 0u)
{
    return
        static_cast<FoliagePackedInstance>(candidateSlot & 0x0FFFu) |
        (static_cast<FoliagePackedInstance>(meshId & 0xFFFFu) << 12u) |
        (static_cast<FoliagePackedInstance>(flags & 0x0Fu) << 28u);
}
