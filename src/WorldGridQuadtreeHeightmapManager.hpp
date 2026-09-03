#pragma once

#include "AppConfig.hpp"
#include "AssetResidency.hpp"
#include "HeightmapNoiseGenerator.hpp"
#include "SubmittedGpuFence.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

class QuadtreeMeshRenderer;

struct HeightmapExtents
{
    float minHeight = 0.0f;
    float maxHeight = 0.0f;
};

struct CpuResidentHeightmapView
{
    WorldGridQuadtreeLeafId leafId{};
    std::uint16_t sliceIndex = 0;
    std::span<const float> samples{};
};

class WorldGridQuadtreeHeightmapManager
{
public:
    static constexpr std::uint16_t kCapacity = static_cast<std::uint16_t>(AppConfig::Terrain::kHeightmapSliceCapacity);
    static constexpr std::uint16_t kUnavailable = kCapacity;

    WorldGridQuadtreeHeightmapManager();

    void ageMap();
    [[nodiscard]] std::uint16_t requestAsset(
        const WorldGridQuadtreeLeafId& leafId,
        std::uint16_t hint = kUnavailable);
    bool makeCpuResident(const WorldGridQuadtreeLeafId& leafId, QuadtreeMeshRenderer& meshRenderer);
    void requestLeaf(const WorldGridQuadtreeLeafId& leafId, QuadtreeMeshRenderer& meshRenderer);
    void scheduleQueuedGenerations(QuadtreeMeshRenderer& meshRenderer);
    void markSubmitted(GenerationJobHandle job, const std::shared_ptr<SubmittedGpuFence>& fence);
    void collectCompletedCpuReadbacks(QuadtreeMeshRenderer& meshRenderer);
    void applyGeneratedExtents(const WorldGridQuadtreeLeafId& leafId, std::uint16_t sliceIndex, const HeightmapExtents& extents, GenerationJobHandle job);
    void clearCache();
    void shutdownAfterGpuIdle();
    [[nodiscard]] bool getExtents(const WorldGridQuadtreeLeafId& leafId, HeightmapExtents& extents) const;
    [[nodiscard]] bool getResidentSliceIndex(const WorldGridQuadtreeLeafId& leafId, std::uint16_t& sliceIndex) const;
    [[nodiscard]] bool tryGetCpuResidentHeightmap(const WorldGridQuadtreeLeafId& leafId, CpuResidentHeightmapView& view) const;
    [[nodiscard]] TerrainNoiseSettings& terrainSettings() { return m_noiseGenerator.settings(); }
    [[nodiscard]] const TerrainNoiseSettings& terrainSettings() const { return m_noiseGenerator.settings(); }
    [[nodiscard]] std::uint16_t computeDispatchBudget() const { return m_computeDispatchBudget; }
    void setComputeDispatchBudget(std::uint16_t budget);
    [[nodiscard]] std::uint16_t residentCount() const { return m_residentCount; }
    [[nodiscard]] std::uint16_t queuedCount() const { return static_cast<std::uint16_t>(m_generationJobs.count()); }

private:
    struct LeafIdHash
    {
        [[nodiscard]] std::size_t operator()(const WorldGridQuadtreeLeafId& leafId) const;
    };

    struct HeightmapGenerationJob
    {
        WorldGridQuadtreeLeafId assetId{};
        std::uint16_t targetSlot = kUnavailable;
        TerrainNoiseSettings settings{};
    };

    using HeightmapCache = FixedAssetCache<WorldGridQuadtreeLeafId, std::uint16_t, LeafIdHash>;
    using HeightmapQueue = GenerationQueue<HeightmapGenerationJob, SubmittedGpuFence>;

    [[nodiscard]] std::optional<std::uint16_t> findSlot(const WorldGridQuadtreeLeafId& leafId) const;
    void invalidateSlotMetadata(std::uint16_t slot);

    HeightmapCache m_heightmaps;
    HeightmapQueue m_generationJobs;
    std::array<HeightmapExtents, kCapacity> m_knownExtents{};
    std::array<bool, kCapacity> m_knownExtentsValid{};
    std::array<std::vector<float>, kCapacity> m_cpuHeightmapSamples{};
    std::array<WorldGridQuadtreeLeafId, kCapacity> m_cpuHeightmapLeafIds{};
    std::array<bool, kCapacity> m_cpuHeightmapValid{};
    std::array<bool, kCapacity> m_cpuHeightmapPending{};
    std::uint16_t m_residentCount = 0;
    std::uint16_t m_computeDispatchBudget = 4;
    HeightmapNoiseGenerator m_noiseGenerator;
};
