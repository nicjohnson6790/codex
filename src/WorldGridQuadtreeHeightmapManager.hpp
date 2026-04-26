#pragma once

#include "AppConfig.hpp"
#include "HeightmapNoiseGenerator.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <array>
#include <cstdint>

class QuadtreeMeshRenderer;

struct HeightmapExtents
{
    float minHeight = 0.0f;
    float maxHeight = 0.0f;
};

struct ResidentMapEntry
{
    WorldGridQuadtreeLeafId leafId{};
    std::uint16_t lruSlice = 0;
    std::uint8_t age = 0;
};

class WorldGridQuadtreeHeightmapManager
{
public:
    static constexpr std::uint16_t kCapacity = static_cast<std::uint16_t>(AppConfig::Terrain::kHeightmapSliceCapacity);

    WorldGridQuadtreeHeightmapManager();

    void ageMap();
    bool makeResident(const WorldGridQuadtreeLeafId& leafId);
    void requestLeaf(const WorldGridQuadtreeLeafId& leafId, QuadtreeMeshRenderer& meshRenderer);
    void dispatchFromQueue(QuadtreeMeshRenderer& meshRenderer);
    void applyGeneratedExtents(
        const WorldGridQuadtreeLeafId& leafId,
        std::uint16_t sliceIndex,
        const HeightmapExtents& extents);
    void clearCache();
    [[nodiscard]] bool getExtents(const WorldGridQuadtreeLeafId& leafId, HeightmapExtents& extents) const;
    [[nodiscard]] TerrainNoiseSettings& terrainSettings() { return m_noiseGenerator.settings(); }
    [[nodiscard]] const TerrainNoiseSettings& terrainSettings() const { return m_noiseGenerator.settings(); }
    [[nodiscard]] std::uint16_t computeDispatchBudget() const { return m_computeDispatchBudget; }
    void setComputeDispatchBudget(std::uint16_t budget);
    [[nodiscard]] std::uint16_t residentCount() const { return m_residentCount; }
    [[nodiscard]] std::uint16_t queuedCount() const { return m_queueCount; }

private:
    [[nodiscard]] std::uint16_t findResidentIndex(const WorldGridQuadtreeLeafId& leafId) const;
    [[nodiscard]] bool queueContains(const WorldGridQuadtreeLeafId& leafId) const;
    bool enqueueLeaf(const WorldGridQuadtreeLeafId& leafId);
    [[nodiscard]] bool dequeueLeaf(WorldGridQuadtreeLeafId& leafId);
    [[nodiscard]] std::uint16_t findOldestResidentIndex() const;

    std::array<WorldGridQuadtreeLeafId, kCapacity> m_leafQueue{};
    std::uint16_t m_queueStart = 0;
    std::uint16_t m_queueEnd = 0;
    std::uint16_t m_queueCount = 0;

    std::array<ResidentMapEntry, kCapacity> m_residentMap{};
    std::uint16_t m_residentCount = 0;

    std::array<HeightmapExtents, kCapacity> m_knownExtents{};
    std::array<bool, kCapacity> m_knownExtentsValid{};
    std::array<std::uint16_t, kCapacity> m_freeSlots{};
    std::uint16_t m_freeSlotCount = 0;
    std::uint16_t m_computeDispatchBudget = 4;

    HeightmapNoiseGenerator m_noiseGenerator;
};
