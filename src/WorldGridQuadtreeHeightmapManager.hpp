#pragma once

#include "AppConfig.hpp"
#include "AssetResidency.hpp"
#include "HeightmapDataset.hpp"
#include "Position.hpp"
#include "SubmittedGpuFence.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <span>
#include <string>
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

struct FinalSourceReference
{
    SourceHeightmapId sourceHeightmapId = 0;
    std::int32_t tileX = 0;
    std::int32_t tileY = 0;
    std::uint64_t revision = 0;
    std::uint16_t lastHint = UINT16_MAX;
};

struct HeightmapSourceGpuDescriptor
{
    glm::vec4 uvOriginAndHeightScale{0.0f}; // sample-space x/y at destination origin, height scale
    glm::vec4 uvSteps{0.0f};                // step for destination X (xy), destination Y (zw)
    glm::vec4 ownershipBounds{0.0f};        // min x/y, max x/y in dataset sample coordinates
    glm::uvec4 source{0u};                  // source slice, outer-min flags x/y
};

class WorldGridQuadtreeHeightmapManager
{
  public:
    static constexpr std::uint16_t kCapacity = static_cast<std::uint16_t>(AppConfig::Terrain::kHeightmapSliceCapacity);
    static constexpr std::uint16_t kUnavailable = kUnavailableCacheIndex;
    static constexpr std::uint16_t kSourceTileCapacity = static_cast<std::uint16_t>(AppConfig::Terrain::kSourceHeightmapCacheCapacity);
    static constexpr std::uint16_t kSourceUnavailable = kSourceTileCapacity;
    static constexpr std::uint16_t kMaxFinalHeightmapsPerDispatch =
        static_cast<std::uint16_t>(AppConfig::Terrain::kMaxFinalHeightmapsPerDispatch);
    static constexpr std::uint32_t kSourceDescriptorCapacity = AppConfig::Terrain::kSourceHeightmapDescriptorCapacity;
    static_assert(kSourceTileCapacity > 0 && kSourceTileCapacity < UINT16_MAX);
    static_assert(AppConfig::Terrain::kSourceHeightmapTileResolution == RuntimeAssets::kHeightmapTileResolution);
    static_assert(AppConfig::Terrain::kSourceHeightmapTileIntervalCount == RuntimeAssets::kHeightmapTileStride);

    struct Diagnostics
    {
        std::uint32_t sourceOccupied = 0, sourceReady = 0, sourceLoading = 0, sourceAgeZero = 0;
        std::uint32_t sourceHashOccupied = 0, sourceHashCapacity = 0, sourceHashCollisions = 0;
        std::uint64_t sourceHits = 0, sourceMisses = 0, sourceLoads = 0, sourceUploads = 0, sourceEvictions = 0;
        std::uint32_t referenceCount = 0, referenceCapacity = 0, referenceHighWater = 0;
        std::uint32_t waitingFinals = 0, pendingContributions = 0, discardedJobs = 0;
        std::uint32_t lastSourceDescriptors = 0, lastFinalGenerations = 0, submittedJobs = 0, descriptorOverflows = 0;
        std::uint32_t queuedGenerationJobs = 0;
        std::uint64_t completedFinalGenerations = 0;
        std::uint64_t completedFinalGenerationsWithSources = 0;
    };

    WorldGridQuadtreeHeightmapManager();
    bool addDataset(std::shared_ptr<HeightmapDataset> dataset);
    bool addSourceHeightmap(SourceHeightmap source);
    bool updateSourceHeightmap(SourceHeightmap source);
    bool removeSourceHeightmap(SourceHeightmapId sourceHeightmapId);
    void invalidateSourceTile(const SourceTileId &id);
    void ageMap();
    [[nodiscard]] std::uint16_t requestAsset(const WorldGridQuadtreeLeafId &leafId, std::uint16_t hint = kUnavailable);
    [[nodiscard]] CacheIndex requestCpuAsset(const WorldGridQuadtreeLeafId &leafId, QuadtreeMeshRenderer &meshRenderer,
                                            CacheIndex hint = kUnavailable);
    void requestLeaf(const WorldGridQuadtreeLeafId &leafId, QuadtreeMeshRenderer &meshRenderer);
    void scheduleQueuedGenerations(QuadtreeMeshRenderer &meshRenderer);
    void markSubmitted(GenerationJobHandle job, const std::shared_ptr<SubmittedGpuFence> &fence);
    void markSourceUploadsSubmitted(std::span<const std::uint16_t> slots, const std::shared_ptr<SubmittedGpuFence> &fence);
    void collectCompletedCpuReadbacks(QuadtreeMeshRenderer &meshRenderer);
    void applyGeneratedExtents(const WorldGridQuadtreeLeafId &, std::uint16_t, const HeightmapExtents &, GenerationJobHandle);
    void clearCache();
    void shutdownAfterGpuIdle();
    [[nodiscard]] CacheIndex isResident(const WorldGridQuadtreeLeafId &, CacheIndex hint = kUnavailable) const;
    [[nodiscard]] bool buildExtents(const WorldGridQuadtreeLeafId &, CacheIndex, HeightmapExtents &) const;
    [[nodiscard]] bool buildCpuResidentHeightmap(const WorldGridQuadtreeLeafId &, CacheIndex, CpuResidentHeightmapView &) const;
    [[nodiscard]] std::uint16_t computeDispatchBudget() const
    {
        return m_computeDispatchBudget;
    }
    void setComputeDispatchBudget(std::uint16_t budget);
    [[nodiscard]] std::uint16_t residentCount() const
    {
        return m_residentCount;
    }
    [[nodiscard]] std::uint16_t queuedCount() const
    {
        return static_cast<std::uint16_t>(m_generationJobs.count());
    }
    [[nodiscard]] Diagnostics diagnostics() const;

  private:
    struct LeafIdHash
    {
        std::size_t operator()(const WorldGridQuadtreeLeafId &) const;
    };
    struct SourceTileHash
    {
        std::size_t operator()(const SourceTileId &) const;
    };
    struct FinalHeightmapGenerationJob
    {
        std::uint16_t finalCacheSlot = kUnavailable;
    };
    struct SourceTileLoadResult
    {
        std::vector<float> samples;
        std::string error;
    };
    struct SourceUpload
    {
        std::future<SourceTileLoadResult> load;
        std::vector<float> samples;
        std::uint64_t revision = 0;
        bool queuedToRenderer = false;
        std::shared_ptr<SubmittedGpuFence> fence;
    };
    struct FinalMetadata
    {
        std::uint32_t referenceFront = 0;
        std::uint32_t referenceCount = 0;
    };

    using HeightmapCache = FixedAssetCache<WorldGridQuadtreeLeafId, std::uint16_t, LeafIdHash>;
    using SourceCache = FixedAssetCache<SourceTileId, std::uint16_t, SourceTileHash>;
    using HeightmapQueue = GenerationQueue<FinalHeightmapGenerationJob, SubmittedGpuFence>;
    std::optional<std::uint16_t> findSlot(const WorldGridQuadtreeLeafId &) const;
    const SourceHeightmap *findSource(SourceHeightmapId) const;
    const HeightmapDataset *findDataset(HeightmapDatasetId) const;
    void allocateReferences(std::uint16_t finalSlot, const WorldGridQuadtreeLeafId &leafId);
    bool makeResident(std::uint16_t finalSlot);
    std::uint16_t requestSourceTile(const SourceTileId &, std::uint64_t revision, std::uint16_t hint);
    std::optional<std::uint16_t> findSourceAllocationCandidate();
    bool buildDescriptors(std::uint16_t finalSlot, std::vector<HeightmapSourceGpuDescriptor> &out);
    void compactReferences();
    void discardCurrentJob(std::uint16_t finalSlot);
    void invalidateSlotMetadata(std::uint16_t slot);

    HeightmapCache m_heightmaps;
    SourceCache m_sourceTiles;
    HeightmapQueue m_generationJobs;
    std::array<SourceUpload, kSourceTileCapacity> m_sourceUploads{};
    std::array<std::vector<std::shared_ptr<SubmittedGpuFence>>, kSourceTileCapacity> m_sourceReadFences{};
    std::array<std::uint64_t, kSourceTileCapacity> m_sourceRevisions{};
    std::array<FinalMetadata, kCapacity> m_finalMetadata{};
    std::vector<FinalSourceReference> m_sourceReferences;
    std::vector<std::shared_ptr<HeightmapDataset>> m_datasets;
    std::vector<SourceHeightmap> m_sources;
    std::array<HeightmapExtents, kCapacity> m_knownExtents{};
    std::array<bool, kCapacity> m_knownExtentsValid{};
    std::array<std::vector<float>, kCapacity> m_cpuHeightmapSamples{};
    std::array<WorldGridQuadtreeLeafId, kCapacity> m_cpuHeightmapLeafIds{};
    std::array<bool, kCapacity> m_cpuHeightmapValid{}, m_cpuHeightmapPending{};
    std::uint16_t m_residentCount = 0, m_computeDispatchBudget = 4;
    mutable Diagnostics m_stats{};
};
