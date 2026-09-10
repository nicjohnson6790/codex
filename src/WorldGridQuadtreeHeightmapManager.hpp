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
    glm::vec4 quantization{0.0f};           // per-tile scale/bias; yScale remains independent
};

class WorldGridQuadtreeHeightmapManager
{
  public:
    static constexpr std::uint16_t kCapacity = static_cast<std::uint16_t>(AppConfig::Terrain::kHeightmapSliceCapacity);
    static constexpr std::uint16_t kUnavailable = kUnavailableCacheIndex;
    static constexpr std::uint16_t kCpuCapacity = AppConfig::Terrain::kCpuHeightmapCacheCapacity;
    static constexpr std::size_t kFinalSampleCount = AppConfig::Terrain::kHeightmapResolution * AppConfig::Terrain::kHeightmapResolution;
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
        std::uint32_t cpuOccupied = 0, cpuReady = 0, cpuLoading = 0, cpuCapacity = kCpuCapacity;
        std::uint32_t readbacks = 0, readbackCapacity = AppConfig::Terrain::kHeightmapReadbackCapacity, readbackHighWater = 0;
        std::uint64_t cpuMisses = 0, cpuEvictions = 0, cpuCacheBlocked = 0, readbackBlocked = 0;
        std::uint64_t cpuCompleted = 0, cpuDiscarded = 0, cpuStaleRetired = 0;
        std::uint64_t sourceStagingBlocked = 0, sourceCacheBlocked = 0, sourceUploadBlocked = 0, referenceOverflows = 0;
        bool sourceStagingBusy = false;
        std::uint32_t sourceCapacity = kSourceTileCapacity;
        std::uint32_t sourceDecodeBatchHighWater = 0, sourceUploadBatchHighWater = 0;
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
    // Submit one complete parent, including topology resolved by the quadtree.
    [[nodiscard]] CacheIndex requestLeaf(const WorldGridQuadtreeLeafId &leafId, CacheIndex hint,
        const std::array<glm::uvec4, 4>& bridges, QuadtreeMeshRenderer &meshRenderer);
    void scheduleQueuedGenerations(QuadtreeMeshRenderer &meshRenderer);
    void markSubmitted(GenerationJobHandle job, const std::shared_ptr<SubmittedGpuFence> &fence);
    void markSourceUploadsSubmitted(std::span<const std::uint16_t> slots, const std::shared_ptr<SubmittedGpuFence> &fence);
    void collectCompletedCpuReadbacks(QuadtreeMeshRenderer &meshRenderer);
    void markCpuReadbackSubmitted(GenerationJobHandle, const std::shared_ptr<SubmittedGpuFence> &);
    void completeCpuReadback(GenerationJobHandle, std::span<const float>);
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
    // Explicit command-line GPU stress validation; never used by normal traversal.
    void stressResidencyForValidation(QuadtreeMeshRenderer &, std::uint64_t frame);
    [[nodiscard]] Position traversalPositionForValidation(std::uint64_t stop) const;

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
        CacheIndex slot = kSourceUnavailable;
        HeightmapDataset::TileQuantization quantization;
        bool success = false;
        std::string error;
    };
    struct SourceDecodeJob
    {
        std::shared_ptr<HeightmapDataset> dataset;
        SourceTileId id;
        CacheIndex slot = kSourceUnavailable;
        std::span<std::int16_t> destination;
    };
    struct SourceLoadBatchResult
    {
        std::array<SourceTileLoadResult, kSourceTileCapacity> tiles;
        std::uint16_t count = 0;
    };
    struct SourceUpload
    {
        std::uint64_t revision = 0;
        bool decoding = false;
        bool queuedToRenderer = false;
        std::shared_ptr<SubmittedGpuFence> fence;
    };
    struct FinalMetadata
    {
        std::uint32_t referenceCount = 0;
        bool overflow = false;
    };
    struct CpuReadbackJob
    {
        WorldGridQuadtreeLeafId leafId;
        CacheIndex cpuSlot, finalSlot;
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
    void invalidateCpuForFinal(std::uint16_t finalSlot);
    void discardCurrentJob(std::uint16_t finalSlot);
    void invalidateSlotMetadata(std::uint16_t slot);

    HeightmapCache m_heightmaps;
    SourceCache m_sourceTiles;
    HeightmapQueue m_generationJobs;
    HeightmapCache m_cpuHeightmaps;
    GenerationQueue<CpuReadbackJob, SubmittedGpuFence> m_cpuReadbacks;
    std::array<CacheIndex, kCpuCapacity> m_cpuFinalSlots{};
    std::unique_ptr<float[]> m_cpuHeightmapSamples;
    std::unique_ptr<std::byte[]> m_sourceCompressedStaging, m_sourceDecompressedStaging;
    std::future<SourceLoadBatchResult> m_sourceLoad;
    std::array<HeightmapDataset::TileQuantization, kSourceTileCapacity> m_sourceQuantization{};
    std::array<SourceUpload, kSourceTileCapacity> m_sourceUploads{};
    std::array<std::vector<std::shared_ptr<SubmittedGpuFence>>, kSourceTileCapacity> m_sourceReadFences{};
    std::array<std::uint64_t, kSourceTileCapacity> m_sourceRevisions{};
    std::array<FinalMetadata, kCapacity> m_finalMetadata{};
    std::unique_ptr<FinalSourceReference[]> m_sourceReferences;
    std::vector<std::shared_ptr<HeightmapDataset>> m_datasets;
    std::vector<SourceHeightmap> m_sources;
    std::array<HeightmapExtents, kCapacity> m_knownExtents{};
    std::array<bool, kCapacity> m_knownExtentsValid{};
    std::uint16_t m_residentCount = 0, m_computeDispatchBudget = 4;
    mutable Diagnostics m_stats{};
};
