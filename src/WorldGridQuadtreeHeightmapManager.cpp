#include "WorldGridQuadtreeHeightmapManager.hpp"

#include "PerformanceCapture.hpp"
#include "QuadtreeMeshRenderer.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <vector>

namespace
{
std::uint64_t mix64(std::uint64_t x)
{
    x ^= x >> 30U; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27U; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31U);
}
}

std::size_t WorldGridQuadtreeHeightmapManager::LeafIdHash::operator()(const WorldGridQuadtreeLeafId& leafId) const
{
    std::uint64_t hash = 0x9e3779b97f4a7c15ULL;
    hash = mix64(hash ^ mix64(std::bit_cast<std::uint64_t>(leafId.gridX) + 0x9e3779b97f4a7c15ULL));
    hash = mix64(hash ^ mix64(std::bit_cast<std::uint64_t>(leafId.gridY) + 0xbf58476d1ce4e5b9ULL));
    return static_cast<std::size_t>(mix64(hash ^ mix64(leafId.subdivisionPath + 0x94d049bb133111ebULL)));
}

WorldGridQuadtreeHeightmapManager::WorldGridQuadtreeHeightmapManager()
    : m_heightmaps(kCapacity, 256, 8)
    , m_generationJobs(kCapacity)
{
    clearCache();
}

void WorldGridQuadtreeHeightmapManager::ageMap()
{
    m_generationJobs.retireSignaledDiscarded();
    m_heightmaps.age();
}

std::optional<std::uint16_t> WorldGridQuadtreeHeightmapManager::findSlot(const WorldGridQuadtreeLeafId& leafId) const
{
    return m_heightmaps.find(leafId);
}

std::uint16_t WorldGridQuadtreeHeightmapManager::requestAsset(
    const WorldGridQuadtreeLeafId& leafId,
    std::uint16_t hint)
{
    std::optional<std::uint16_t> existing;
    if (hint != kUnavailable && m_heightmaps.validatesHint(hint, leafId)) existing = hint;
    else existing = findSlot(leafId);

    if (existing)
    {
        m_heightmaps.touch(*existing);
        return m_heightmaps.isReady(*existing) ? *existing : kUnavailable;
    }

    const auto candidate = m_heightmaps.findAllocationCandidate();
    if (!candidate) return kUnavailable;

    const auto newJob = m_generationJobs.tryPush({ leafId, *candidate, m_noiseGenerator.settings() });
    if (!newJob) return kUnavailable;

    if (m_heightmaps.isOpen(*candidate))
    {
        const GenerationJobHandle oldJob = m_heightmaps.activeJob(*candidate);
        if (oldJob.valid() && m_generationJobs.contains(oldJob)) m_generationJobs.discard(oldJob);
        invalidateSlotMetadata(*candidate);
    }
    else
    {
        ++m_residentCount;
    }

    m_heightmaps.assign(*candidate, leafId);
    m_heightmaps.setActiveJob(*candidate, *newJob);
    return kUnavailable;
}

bool WorldGridQuadtreeHeightmapManager::makeCpuResident(const WorldGridQuadtreeLeafId& leafId, QuadtreeMeshRenderer& meshRenderer)
{
    const std::uint16_t slot = requestAsset(leafId);
    if (slot == kUnavailable) return false;
    if (m_cpuHeightmapValid[slot] && m_cpuHeightmapLeafIds[slot] == leafId) return true;
    if (!m_cpuHeightmapPending[slot])
        m_cpuHeightmapPending[slot] = meshRenderer.requestHeightmapSliceDownload(leafId, slot);
    return false;
}

void WorldGridQuadtreeHeightmapManager::requestLeaf(const WorldGridQuadtreeLeafId& leafId, QuadtreeMeshRenderer& meshRenderer)
{
    const std::uint16_t slot = requestAsset(leafId);
    if (slot != kUnavailable) meshRenderer.addLeaf(leafId, slot);
}

bool WorldGridQuadtreeHeightmapManager::getExtents(const WorldGridQuadtreeLeafId& leafId, HeightmapExtents& extents) const
{
    const auto slot = findSlot(leafId);
    if (!slot || !m_heightmaps.isReady(*slot) || !m_knownExtentsValid[*slot]) return false;
    extents = m_knownExtents[*slot];
    return true;
}

bool WorldGridQuadtreeHeightmapManager::getResidentSliceIndex(const WorldGridQuadtreeLeafId& leafId, std::uint16_t& sliceIndex) const
{
    const auto slot = findSlot(leafId);
    if (!slot || !m_heightmaps.isReady(*slot)) return false;
    sliceIndex = *slot;
    return true;
}

bool WorldGridQuadtreeHeightmapManager::tryGetCpuResidentHeightmap(const WorldGridQuadtreeLeafId& leafId, CpuResidentHeightmapView& view) const
{
    std::uint16_t slot = 0;
    if (!getResidentSliceIndex(leafId, slot) || !m_cpuHeightmapValid[slot] || m_cpuHeightmapLeafIds[slot] != leafId || m_cpuHeightmapSamples[slot].empty())
        return false;
    view = { leafId, slot, std::span<const float>(m_cpuHeightmapSamples[slot]) };
    return true;
}

void WorldGridQuadtreeHeightmapManager::collectCompletedCpuReadbacks(QuadtreeMeshRenderer& meshRenderer)
{
    std::vector<QuadtreeMeshRenderer::CompletedHeightmapSliceReadback> completed;
    meshRenderer.collectCompletedHeightmapSliceReadbacks(completed);
    for (const auto& readback : completed)
    {
        const auto slot = findSlot(readback.leafId);
        if (!slot || *slot != readback.sliceIndex || !m_heightmaps.isReady(*slot)) continue;
        m_cpuHeightmapSamples[*slot].assign(readback.samples.begin(), readback.samples.end());
        m_cpuHeightmapLeafIds[*slot] = readback.leafId;
        m_cpuHeightmapValid[*slot] = true;
        m_cpuHeightmapPending[*slot] = false;
    }
}

void WorldGridQuadtreeHeightmapManager::applyGeneratedExtents(const WorldGridQuadtreeLeafId& leafId, std::uint16_t slot, const HeightmapExtents& extents, GenerationJobHandle job)
{
    if (!m_generationJobs.contains(job) || !m_generationJobs.isSubmitted(job) ||
        !m_generationJobs.fence(job) || !m_generationJobs.fence(job)->isSignaled()) return;
    const auto current = findSlot(leafId);
    if (!m_generationJobs.isDiscarded(job) && current && *current == slot && m_heightmaps.activeJob(slot) == job)
    {
        m_knownExtents[slot] = extents;
        m_knownExtentsValid[slot] = true;
        m_heightmaps.markReady(slot);
        m_heightmaps.clearActiveJob(slot, job);
    }
    m_generationJobs.markCompleted(job);
    m_generationJobs.retireCompletedFront();
}

void WorldGridQuadtreeHeightmapManager::setComputeDispatchBudget(std::uint16_t budget)
{
    m_computeDispatchBudget = std::max<std::uint16_t>(1, budget);
}

void WorldGridQuadtreeHeightmapManager::scheduleQueuedGenerations(QuadtreeMeshRenderer& meshRenderer)
{
    HELLO_PROFILE_SCOPE("WorldGridQuadtreeHeightmapManager::ScheduleQueuedGenerations");
    std::uint16_t dispatched = 0;
    m_generationJobs.forEachActive([&](GenerationJobHandle handle) {
        if (dispatched >= m_computeDispatchBudget || m_generationJobs.isCompleted(handle) || m_generationJobs.isDiscarded(handle) ||
            m_generationJobs.isSubmitted(handle)) return;
        const HeightmapGenerationJob job = m_generationJobs.job(handle);
        if (!meshRenderer.queueHeightmapGeneration(job.assetId, job.targetSlot, job.settings, handle)) return;
        ++dispatched;
    });
    m_generationJobs.retireCompletedFront();
}

void WorldGridQuadtreeHeightmapManager::markSubmitted(
    GenerationJobHandle job,
    const std::shared_ptr<SubmittedGpuFence>& fence)
{
    if (m_generationJobs.contains(job)) m_generationJobs.markSubmitted(job, fence);
}

void WorldGridQuadtreeHeightmapManager::invalidateSlotMetadata(std::uint16_t slot)
{
    m_knownExtents[slot] = {};
    m_knownExtentsValid[slot] = false;
    m_cpuHeightmapSamples[slot].clear();
    m_cpuHeightmapLeafIds[slot] = {};
    m_cpuHeightmapValid[slot] = false;
    m_cpuHeightmapPending[slot] = false;
}

void WorldGridQuadtreeHeightmapManager::clearCache()
{
    m_heightmaps.clear();
    m_generationJobs.discardAll();
    m_residentCount = 0;
    for (std::uint16_t slot = 0; slot < kCapacity; ++slot) invalidateSlotMetadata(slot);
}

void WorldGridQuadtreeHeightmapManager::shutdownAfterGpuIdle()
{
    m_heightmaps.clear();
    m_generationJobs.clear();
}
