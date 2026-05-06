#pragma once

#include "FoliageTypes.hpp"
#include "HeightmapNoiseGenerator.hpp"

#include <array>
#include <cstdint>

class FoliageCanopyRenderer;

class WorldGridFoliageCanopyManager
{
public:
    static constexpr std::uint16_t kCapacity = FoliageConfig::kCanopyCellPoolCapacity;

    WorldGridFoliageCanopyManager();

    void ageMap();
    void setTerrainSettings(const TerrainNoiseSettings& settings);
    void setWaterLevel(float waterLevel);
    void clearCache();

    [[nodiscard]] bool makeResident(
        const WorldGridQuadtreeLeafId& leafId,
        const WorldGridQuadtreeLeafId& terrainLeafId,
        std::uint16_t terrainSliceIndex);
    void dispatchFromQueue(FoliageCanopyRenderer& renderer);

    [[nodiscard]] bool getReadyCellInfo(
        const WorldGridQuadtreeLeafId& leafId,
        FoliageCanopyReadyCellInfo& cellInfo) const;

    [[nodiscard]] std::uint16_t residentCount() const { return m_residentCount; }
    [[nodiscard]] std::uint16_t queuedCount() const { return m_queueCount; }
    [[nodiscard]] std::uint16_t readyCount() const;

private:
    enum ResidentFlags : std::uint8_t
    {
        ReadyMask = 1u << 0u,
    };

    struct LookupBucketEntry
    {
        std::uint16_t residentIndex = kCapacity;
    };

    struct LookupOverflowEntry
    {
        WorldGridQuadtreeLeafId leafId{};
        std::uint16_t residentIndex = kCapacity;
        std::uint16_t bucketIndex = 0;
        bool used = false;
    };

    struct QueuedLeafRequest
    {
        WorldGridQuadtreeLeafId leafId{};
        FoliageTerrainSource terrainSource{};
        std::uint32_t requestFrame = 0;
    };

    [[nodiscard]] std::uint16_t findResidentIndex(const WorldGridQuadtreeLeafId& leafId) const;
    void insertResidentLookup(const WorldGridQuadtreeLeafId& leafId, std::uint16_t residentIndex);
    void removeResidentLookup(const WorldGridQuadtreeLeafId& leafId, std::uint16_t residentIndex);
    [[nodiscard]] static std::uint64_t mix64(std::uint64_t x);
    [[nodiscard]] static std::uint64_t hashLeafId(const WorldGridQuadtreeLeafId& leafId);
    [[nodiscard]] static std::uint16_t bucketIndexForLeafId(const WorldGridQuadtreeLeafId& leafId);
    bool enqueueLeaf(const WorldGridQuadtreeLeafId& leafId, const FoliageTerrainSource& terrainSource);
    [[nodiscard]] bool dequeueLeaf(QueuedLeafRequest& request);
    [[nodiscard]] std::uint16_t findOldestEvictableResidentIndex() const;
    void assignResidentCell(std::uint16_t residentIndex, const WorldGridQuadtreeLeafId& leafId);
    void clearResidentCell(std::uint16_t residentIndex);
    void resetCacheState();
    [[nodiscard]] static bool residentHasFlag(const FoliageCanopyResidentCellEntry& entry, std::uint8_t mask);
    static void setResidentFlag(FoliageCanopyResidentCellEntry& entry, std::uint8_t mask, bool enabled);

    std::array<QueuedLeafRequest, kCapacity> m_leafQueue{};
    std::uint16_t m_queueStart = 0;
    std::uint16_t m_queueEnd = 0;
    std::uint16_t m_queueCount = 0;

    std::array<FoliageCanopyResidentCellEntry, kCapacity> m_residentEntries{};
    std::array<bool, kCapacity> m_residentUsed{};
    std::array<bool, kCapacity> m_generationLocked{};
    std::array<FoliageTerrainSource, kCapacity> m_terrainSources{};
    std::array<std::array<LookupBucketEntry, FoliageConfig::kCanopyLookupBucketEntryCount>, FoliageConfig::kCanopyLookupBucketCount>
        m_lookupBuckets{};
    std::array<bool, FoliageConfig::kCanopyLookupBucketCount> m_lookupBucketHasOverflow{};
    std::array<LookupOverflowEntry, FoliageConfig::kCanopyLookupOverflowCapacity> m_lookupOverflowEntries{};
    std::array<std::uint16_t, kCapacity> m_freeResidentIndices{};

    std::uint16_t m_residentCount = 0;
    std::uint16_t m_lookupOverflowCount = 0;
    std::uint16_t m_freeResidentIndexCount = 0;
    std::uint32_t m_requestFrame = 0;
    TerrainNoiseSettings m_terrainSettings = sanitizeTerrainNoiseSettings(TerrainNoiseSettings{});
    float m_waterLevel = AppConfig::Water::kDefaultWaterLevel;
};
