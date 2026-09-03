#pragma once

#include "FoliageTypes.hpp"
#include "HeightmapNoiseGenerator.hpp"
#include "AssetResidency.hpp"
#include "SubmittedGpuFence.hpp"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

class QuadtreeMeshRenderer;
class FoliageImposterRenderer;

class WorldGridFoliageManager
{
public:
    static constexpr std::uint16_t kCapacity = FoliageConfig::kPagePoolCapacity;

    WorldGridFoliageManager();

    void ageMap();
    void setTerrainSettings(const TerrainNoiseSettings& settings);
    void setWaterLevel(float waterLevel);
    void clearCache();
    void shutdownAfterGpuIdle();

    [[nodiscard]] std::uint16_t requestAsset(
        const WorldGridQuadtreeLeafId& leafId,
        const WorldGridQuadtreeLeafId& terrainLeafId,
        std::uint16_t terrainSliceIndex,
        std::uint16_t hint = kCapacity);
    void scheduleQueuedGenerations(QuadtreeMeshRenderer& meshRenderer);
    void markSubmitted(GenerationJobHandle job, const std::shared_ptr<SubmittedGpuFence>& fence);
    void applyGeneratedPageLiveCount(
        const WorldGridQuadtreeLeafId& leafId,
        std::uint16_t pageIndex,
        std::uint16_t liveCount,
        GenerationJobHandle job);

    [[nodiscard]] bool buildReadyPageInfo(
        const WorldGridQuadtreeLeafId& leafId,
        std::uint16_t residentIndex,
        FoliageReadyPageInfo& pageInfo) const;
    [[nodiscard]] bool emitPageDraw(
        const WorldGridQuadtreeLeafId& pageId,
        std::uint16_t residentIndex,
        const WorldGridQuadtreeLeafId& terrainLeafId,
        std::uint16_t terrainSliceIndex,
        FoliageImposterRenderer& foliageRenderer) const;
    [[nodiscard]] bool getReadyPageInfo(
        const WorldGridQuadtreeLeafId& leafId,
        FoliageReadyPageInfo& pageInfo) const;

    [[nodiscard]] std::uint16_t residentCount() const { return m_residentCount; }
    [[nodiscard]] std::uint16_t queuedCount() const { return static_cast<std::uint16_t>(m_generationJobs.count()); }
    [[nodiscard]] std::uint16_t maskPendingCount() const;
    [[nodiscard]] std::uint16_t uploadPendingCount() const;
    [[nodiscard]] std::uint16_t readyCount() const;

private:
    enum ResidentFlags : std::uint8_t
    {
        UploadPendingMask = 1u << 0u,
        ReadyMask = 1u << 1u,
        MaskValidMask = 1u << 2u,
        MaskPendingMask = 1u << 4u,
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
    };

    struct QueuedLeafRequest
    {
        WorldGridQuadtreeLeafId leafId{};
        FoliageTerrainSource terrainSource{};
    };

    [[nodiscard]] std::uint16_t findResidentIndex(const WorldGridQuadtreeLeafId& leafId) const;
    [[nodiscard]] static std::uint64_t mix64(std::uint64_t x);
    [[nodiscard]] static std::uint64_t hashLeafId(const WorldGridQuadtreeLeafId& leafId);
    void assignResidentPage(std::uint16_t residentIndex, const WorldGridQuadtreeLeafId& leafId);
    void clearResidentPage(std::uint16_t residentIndex);
    void resetCacheState();
    [[nodiscard]] bool queueGpuPageGeneration(
        QuadtreeMeshRenderer& meshRenderer,
        std::uint16_t residentIndex);
    [[nodiscard]] static bool residentHasFlag(const FoliageResidentPageEntry& entry, std::uint8_t mask);
    static void setResidentFlag(FoliageResidentPageEntry& entry, std::uint8_t mask, bool enabled);

    FixedAssetCache<WorldGridQuadtreeLeafId, std::uint16_t, LeafIdHash> m_cache;
    GenerationQueue<GenerationJob, SubmittedGpuFence> m_generationJobs;

    std::array<FoliageResidentPageEntry, kCapacity> m_residentEntries{};
    std::array<FoliageTerrainSource, kCapacity> m_terrainSources{};

    std::uint16_t m_residentCount = 0;
    std::uint32_t m_nextContentVersion = 1u;
    TerrainNoiseSettings m_terrainSettings = sanitizeTerrainNoiseSettings(TerrainNoiseSettings{});
    float m_waterLevel = AppConfig::Water::kDefaultWaterLevel;
};
