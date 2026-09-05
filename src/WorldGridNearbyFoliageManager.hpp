#pragma once

#include "AssetResidency.hpp"
#include "FoliageTypes.hpp"
#include "SubmittedGpuFence.hpp"

#include <cstdint>
#include <memory>
#include <vector>

class NearbyFoliageRenderer;

class WorldGridNearbyFoliageManager
{
public:
    static constexpr std::uint16_t kCapacity = FoliageConfig::kNearbyDecodedPageLruCapacity;
    static constexpr std::uint16_t kUnavailable = kUnavailableCacheIndex;

    WorldGridNearbyFoliageManager();

    void age();
    void clear();
    void shutdownAfterGpuIdle();
    [[nodiscard]] std::uint16_t requestAsset(
        const WorldGridQuadtreeLeafId& pageKey,
        const FoliageReadyPageInfo& sourcePageInfo,
        NearbyFoliageRenderer& renderer,
        std::uint16_t hint = kUnavailable);
    [[nodiscard]] CacheIndex isResident(const WorldGridQuadtreeLeafId& id, CacheIndex hint = kUnavailable) const
    {
        return m_cache.isResident(id, hint);
    }
    void markSubmitted(GenerationJobHandle job, const std::shared_ptr<SubmittedGpuFence>& fence);
    [[nodiscard]] bool complete(
        GenerationJobHandle job,
        const WorldGridQuadtreeLeafId& pageKey,
        std::uint16_t targetSlot);
    [[nodiscard]] std::uint32_t residentCount() const;
    [[nodiscard]] std::uint32_t pendingCount() const;

private:
    struct LeafIdHash
    {
        [[nodiscard]] std::size_t operator()(const WorldGridQuadtreeLeafId& leafId) const;
    };

    struct DecodeJob
    {
        WorldGridQuadtreeLeafId assetId{};
        std::uint16_t targetSlot = kUnavailable;
        FoliageReadyPageInfo sourcePageInfo{};
    };

    FixedAssetCache<WorldGridQuadtreeLeafId, std::uint16_t, LeafIdHash> m_cache;
    GenerationQueue<DecodeJob, SubmittedGpuFence> m_jobs;
};
