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

inline constexpr std::uint32_t kCanopyNodeSizeMeters = 2048u;
inline constexpr std::uint32_t kCanopyCellsPerSide = kCanopyNodeSizeMeters / kPageSizeMeters;
inline constexpr std::uint32_t kCanopyCellCountPerNode = kCanopyCellsPerSide * kCanopyCellsPerSide;
inline constexpr std::uint32_t kCanopyBitsetWordCount = kCandidateSlotCount / 32u;
inline constexpr std::uint32_t kCanopyBitsetByteSize = kCanopyBitsetWordCount * sizeof(std::uint32_t);
inline constexpr std::uint16_t kCanopyCellPoolCapacity = 4096u;
inline constexpr std::uint16_t kCanopyLookupBucketCount = 1024u;
inline constexpr std::uint16_t kCanopyLookupBucketEntryCount = 16u;
inline constexpr std::uint16_t kCanopyLookupOverflowCapacity = 4096u;
inline constexpr std::uint16_t kCanopyGenerationBudgetPerFrame = 256u;
inline constexpr std::uint16_t kCanopyMinimumReadyCellCount = 48u;
inline constexpr float kCanopyFadeStartMeters = 7000.0f;
inline constexpr float kCanopyFadeEndMeters = 11000.0f;
inline constexpr float kCanopyEdgeFadeWidthMeters = 384.0f;
inline constexpr std::uint8_t kCanopyFadeInFrameCount = 12u;
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

struct FoliageCanopyResidentCellEntry
{
    WorldGridQuadtreeLeafId leafId{};
    std::uint16_t slotIndex = 0;
    std::uint8_t age = 0;
    std::uint8_t flags = 0;
};

struct FoliageCanopyReadyCellInfo
{
    std::uint16_t slotIndex = 0;
    std::uint32_t seed = 0;
};

struct FoliageCanopyDrawReference
{
    Position patchOrigin{};
    Position terrainLeafOrigin{};
    float patchSizeMeters = 0.0f;
    float terrainLeafSizeMeters = 0.0f;
    std::uint16_t terrainSliceIndex = 0;
    std::uint32_t patchSeed = 0;
    std::uint8_t drawAgeFrames = 0;
    std::array<std::uint8_t, 4> edgeFadeStrengths{};
    std::array<std::uint16_t, FoliageConfig::kCanopyCellCountPerNode> cellSlotIndices{};
    std::array<std::uint32_t, FoliageConfig::kCanopyCellCountPerNode> cellSeeds{};
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
