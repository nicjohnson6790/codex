#pragma once

#include "FoliageTypes.hpp"
#include "HeightmapNoiseGenerator.hpp"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

class QuadtreeMeshRenderer;

class WorldGridFoliageManager
{
public:
    static constexpr std::uint16_t kCapacity = FoliageConfig::kPagePoolCapacity;

    WorldGridFoliageManager();

    void ageMap();
    void setTerrainSettings(const TerrainNoiseSettings& settings);
    void setWaterLevel(float waterLevel);
    void clearCache();

    [[nodiscard]] bool makeResident(
        const WorldGridQuadtreeLeafId& leafId,
        const WorldGridQuadtreeLeafId& terrainLeafId,
        std::uint16_t terrainSliceIndex);
    void scheduleQueuedGenerations(QuadtreeMeshRenderer& meshRenderer);
    void applyGeneratedPageLiveCounts(
        const std::vector<std::pair<WorldGridQuadtreeLeafId, std::uint16_t>>& generatedLiveCounts);

    [[nodiscard]] bool getReadyPageInfo(
        const WorldGridQuadtreeLeafId& leafId,
        FoliageReadyPageInfo& pageInfo) const;

    [[nodiscard]] std::uint16_t residentCount() const { return m_residentCount; }
    [[nodiscard]] std::uint16_t queuedCount() const { return m_queueCount; }
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

    struct LookupBucketEntry
    {
        std::uint16_t residentIndex = kCapacity;
    };

    struct LookupOverflowEntry
    {
        WorldGridQuadtreeLeafId leafId{};
        std::uint16_t residentIndex = kCapacity;
        std::uint8_t bucketIndex = 0;
        bool used = false;
    };

    struct QueuedLeafRequest
    {
        WorldGridQuadtreeLeafId leafId{};
        FoliageTerrainSource terrainSource{};
    };

    [[nodiscard]] std::uint16_t findResidentIndex(const WorldGridQuadtreeLeafId& leafId) const;
    void insertResidentLookup(const WorldGridQuadtreeLeafId& leafId, std::uint16_t residentIndex);
    void removeResidentLookup(const WorldGridQuadtreeLeafId& leafId, std::uint16_t residentIndex);
    [[nodiscard]] static std::uint64_t mix64(std::uint64_t x);
    [[nodiscard]] static std::uint64_t hashLeafId(const WorldGridQuadtreeLeafId& leafId);
    [[nodiscard]] static std::uint8_t bucketIndexForLeafId(const WorldGridQuadtreeLeafId& leafId);
    [[nodiscard]] bool queueContains(const WorldGridQuadtreeLeafId& leafId) const;
    bool enqueueLeaf(const WorldGridQuadtreeLeafId& leafId, const FoliageTerrainSource& terrainSource);
    [[nodiscard]] bool dequeueLeaf(QueuedLeafRequest& request);
    [[nodiscard]] std::uint16_t findOldestEvictableResidentIndex() const;
    void assignResidentPage(std::uint16_t residentIndex, const WorldGridQuadtreeLeafId& leafId);
    void clearResidentPage(std::uint16_t residentIndex);
    void resetCacheState();
    [[nodiscard]] bool queueGpuPageGeneration(
        QuadtreeMeshRenderer& meshRenderer,
        std::uint16_t residentIndex);
    [[nodiscard]] static bool residentHasFlag(const FoliageResidentPageEntry& entry, std::uint8_t mask);
    static void setResidentFlag(FoliageResidentPageEntry& entry, std::uint8_t mask, bool enabled);

    std::array<QueuedLeafRequest, kCapacity> m_leafQueue{};
    std::uint16_t m_queueStart = 0;
    std::uint16_t m_queueEnd = 0;
    std::uint16_t m_queueCount = 0;

    std::array<FoliageResidentPageEntry, kCapacity> m_residentEntries{};
    std::array<bool, kCapacity> m_residentUsed{};
    std::array<FoliageTerrainSource, kCapacity> m_terrainSources{};
    std::array<std::array<LookupBucketEntry, FoliageConfig::kLookupBucketEntryCount>, FoliageConfig::kLookupBucketCount>
        m_lookupBuckets{};
    std::array<bool, FoliageConfig::kLookupBucketCount> m_lookupBucketHasOverflow{};
    std::array<LookupOverflowEntry, FoliageConfig::kLookupOverflowCapacity> m_lookupOverflowEntries{};
    std::array<std::uint16_t, kCapacity> m_freeResidentIndices{};

    std::uint16_t m_residentCount = 0;
    std::uint16_t m_lookupOverflowCount = 0;
    std::uint16_t m_freeResidentIndexCount = 0;
    std::uint32_t m_nextContentVersion = 1u;
    TerrainNoiseSettings m_terrainSettings = sanitizeTerrainNoiseSettings(TerrainNoiseSettings{});
    float m_waterLevel = AppConfig::Water::kDefaultWaterLevel;
};
