#pragma once

#include "FoliageTypes.hpp"
#include "AssetResidency.hpp"
#include "SubmittedGpuFence.hpp"

#include <array>
#include <cstdint>
#include <vector>

class FoliageCanopyRenderer;

class WorldGridFoliageCanopyManager
{
public:
    static constexpr std::uint16_t kCapacity = FoliageConfig::kCanopyCellPoolCapacity;

    static constexpr CacheIndex kUnavailable = kUnavailableCacheIndex;

    WorldGridFoliageCanopyManager();

    void ageMap();
    void setWaterLevel(float waterLevel);
    void clearCache();
    void shutdownAfterGpuIdle();

    [[nodiscard]] std::uint16_t requestAsset(
        const WorldGridQuadtreeLeafId& leafId,
        const WorldGridQuadtreeLeafId& terrainLeafId,
        std::uint16_t terrainSliceIndex,
        std::uint16_t hint = kUnavailable);
    void scheduleQueuedGenerations(FoliageCanopyRenderer& renderer);
    void markSubmitted(GenerationJobHandle job, const std::shared_ptr<SubmittedGpuFence>& fence);

    [[nodiscard]] bool buildReadyCellInfo(
        const WorldGridQuadtreeLeafId& leafId,
        std::uint16_t residentIndex,
        FoliageCanopyReadyCellInfo& cellInfo) const;
    void emitCanopyDraw(
        const WorldGridQuadtreeLeafId& nodeId,
        std::uint16_t terrainSliceIndex,
        const WorldGridQuadtreeLeafId* cellIds,
        const std::array<std::uint16_t, FoliageConfig::kCanopyCellCountPerNode>& residentIndices,
        std::uint32_t cellCount,
        std::uint32_t readyCellCount,
        std::uint8_t drawAgeFrames,
        const std::array<std::uint8_t, 4>& edgeFadeStrengths,
        FoliageCanopyRenderer& renderer);
    [[nodiscard]] CacheIndex isResident(
        const WorldGridQuadtreeLeafId& leafId, CacheIndex hint = kUnavailable) const;
    void noteRenderedCell(const WorldGridQuadtreeLeafId& leafId);

    [[nodiscard]] std::uint16_t residentCount() const { return m_residentCount; }
    [[nodiscard]] std::uint16_t queuedCount() const { return static_cast<std::uint16_t>(m_generationJobs.count()); }
    [[nodiscard]] std::uint16_t readyCount() const;

private:
    enum ResidentFlags : std::uint8_t
    {
        ReadyMask = 1u << 0u,
    };

    struct LeafIdHash
    {
        [[nodiscard]] std::size_t operator()(const WorldGridQuadtreeLeafId& leafId) const;
    };

    struct GenerationJob
    {
        WorldGridQuadtreeLeafId assetId{};
        std::uint16_t targetSlot = kCapacity;
        FoliageTerrainSource terrainSource{};
        std::uint32_t requestFrame = 0;
    };

    struct QueuedLeafRequest
    {
        WorldGridQuadtreeLeafId leafId{};
        FoliageTerrainSource terrainSource{};
        std::uint32_t requestFrame = 0;
    };

    [[nodiscard]] std::uint16_t findResidentIndex(const WorldGridQuadtreeLeafId& leafId) const;
    [[nodiscard]] static std::uint64_t mix64(std::uint64_t x);
    [[nodiscard]] static std::uint64_t hashLeafId(const WorldGridQuadtreeLeafId& leafId);
    void assignResidentCell(std::uint16_t residentIndex, const WorldGridQuadtreeLeafId& leafId);
    void clearResidentCell(std::uint16_t residentIndex, bool removeFromActiveSet = true);
    void resetCacheState();
    [[nodiscard]] static bool residentHasFlag(const FoliageCanopyResidentCellEntry& entry, std::uint8_t mask);
    static void setResidentFlag(FoliageCanopyResidentCellEntry& entry, std::uint8_t mask, bool enabled);

    FixedAssetCache<WorldGridQuadtreeLeafId, std::uint16_t, LeafIdHash> m_cache;
    GenerationQueue<GenerationJob, SubmittedGpuFence> m_generationJobs;

    std::array<FoliageCanopyResidentCellEntry, kCapacity> m_residentEntries{};
    std::array<FoliageTerrainSource, kCapacity> m_terrainSources{};

    std::uint16_t m_residentCount = 0;
    std::uint32_t m_requestFrame = 0;
    float m_waterLevel = AppConfig::Water::kDefaultWaterLevel;
};
