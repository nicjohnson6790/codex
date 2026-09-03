#include "WorldGridNearbyFoliageManager.hpp"

#include "NearbyFoliageRenderer.hpp"

#include <bit>

namespace
{
std::uint64_t mix64(std::uint64_t value)
{
    value ^= value >> 30U; value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U; value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}
}

std::size_t WorldGridNearbyFoliageManager::LeafIdHash::operator()(const WorldGridQuadtreeLeafId& leafId) const
{
    return static_cast<std::size_t>(mix64(
        mix64(std::bit_cast<std::uint64_t>(leafId.gridX)) ^
        mix64(std::bit_cast<std::uint64_t>(leafId.gridY)) ^
        mix64(leafId.subdivisionPath)));
}

WorldGridNearbyFoliageManager::WorldGridNearbyFoliageManager()
    : m_cache(kCapacity, 256, 8)
    , m_jobs(FoliageConfig::kNearbyReadbackSlotCount)
{
}

void WorldGridNearbyFoliageManager::age()
{
    m_jobs.retireSignaledDiscarded();
    m_cache.age();
}

void WorldGridNearbyFoliageManager::clear()
{
    m_cache.clear();
    m_jobs.discardAll();
}

void WorldGridNearbyFoliageManager::shutdownAfterGpuIdle()
{
    m_cache.clear();
    m_jobs.clear();
}

std::uint16_t WorldGridNearbyFoliageManager::requestAsset(
    const WorldGridQuadtreeLeafId& pageKey,
    const FoliageReadyPageInfo& sourcePageInfo,
    NearbyFoliageRenderer& renderer,
    std::uint16_t hint)
{
    std::optional<std::uint16_t> existing;
    if (hint != kUnavailable && m_cache.validatesHint(hint, pageKey)) existing = hint;
    else existing = m_cache.find(pageKey);

    if (existing)
    {
        m_cache.touch(*existing);
        if (renderer.decodedSlotMatches(*existing, pageKey, sourcePageInfo))
            return m_cache.isReady(*existing) ? *existing : kUnavailable;
    }

    const auto candidate = existing ? existing : m_cache.findAllocationCandidate();
    if (!candidate) return kUnavailable;
    const auto job = m_jobs.tryPush({ pageKey, *candidate, sourcePageInfo });
    if (!job) return kUnavailable;
    if (!renderer.queueDecodedPageGeneration(*candidate, pageKey, sourcePageInfo, *job))
    {
        m_jobs.discard(*job);
        m_jobs.retireCompletedFront();
        return kUnavailable;
    }

    if (m_cache.isOpen(*candidate))
    {
        const GenerationJobHandle oldJob = m_cache.activeJob(*candidate);
        if (oldJob.valid() && m_jobs.contains(oldJob)) m_jobs.discard(oldJob);
    }
    m_cache.assign(*candidate, pageKey);
    m_cache.setActiveJob(*candidate, *job);
    renderer.assignDecodedPageSlot(*candidate, pageKey, sourcePageInfo);
    return kUnavailable;
}

void WorldGridNearbyFoliageManager::markSubmitted(
    GenerationJobHandle job,
    const std::shared_ptr<SubmittedGpuFence>& fence)
{
    if (m_jobs.contains(job)) m_jobs.markSubmitted(job, fence);
}

bool WorldGridNearbyFoliageManager::complete(
    GenerationJobHandle job,
    const WorldGridQuadtreeLeafId& pageKey,
    std::uint16_t targetSlot)
{
    if (!m_jobs.contains(job) || !m_jobs.isSubmitted(job) || !m_jobs.fence(job) || !m_jobs.fence(job)->isSignaled())
        return false;
    const bool accepted = !m_jobs.isDiscarded(job) && m_cache.isOpen(targetSlot) &&
        m_cache.assetId(targetSlot) == pageKey && m_cache.activeJob(targetSlot) == job;
    if (accepted)
    {
        m_cache.markReady(targetSlot);
        m_cache.clearActiveJob(targetSlot, job);
    }
    m_jobs.markCompleted(job);
    m_jobs.retireCompletedFront();
    return accepted;
}

std::uint32_t WorldGridNearbyFoliageManager::residentCount() const
{
    std::uint32_t count = 0;
    for (std::uint16_t slot = 0; slot < kCapacity; ++slot) if (m_cache.isReady(slot)) ++count;
    return count;
}

std::uint32_t WorldGridNearbyFoliageManager::pendingCount() const
{
    return static_cast<std::uint32_t>(m_jobs.count());
}
