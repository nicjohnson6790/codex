#include "WorldGridFoliageManager.hpp"

#include "FoliageImposterRenderer.hpp"
#include "PerformanceCapture.hpp"
#include "QuadtreeMeshRenderer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

WorldGridFoliageManager::WorldGridFoliageManager()
    : m_cache(kCapacity, FoliageConfig::kLookupBucketCount, FoliageConfig::kLookupBucketEntryCount)
    , m_generationJobs(kCapacity)
{
    resetCacheState();
}

void WorldGridFoliageManager::ageMap()
{
    m_generationJobs.retireSignaledDiscarded();
    m_cache.age();
    for (std::uint16_t residentIndex = 0; residentIndex < kCapacity; ++residentIndex)
    {
        if (!m_cache.isOpen(residentIndex))
        {
            continue;
        }

    }
}

void WorldGridFoliageManager::setWaterLevel(float waterLevel)
{
    if (m_waterLevel != waterLevel)
    {
        m_waterLevel = waterLevel;
        clearCache();
    }
}

void WorldGridFoliageManager::clearCache()
{
    resetCacheState();
}

void WorldGridFoliageManager::shutdownAfterGpuIdle()
{
    m_cache.clear();
    m_generationJobs.clear();
}

std::uint16_t WorldGridFoliageManager::requestAsset(
    const WorldGridQuadtreeLeafId& leafId,
    const WorldGridQuadtreeLeafId& terrainLeafId,
    std::uint16_t terrainSliceIndex,
    std::uint16_t hint)
{
    const FoliageTerrainSource terrainSource{
        .terrainLeafId = terrainLeafId,
        .terrainSliceIndex = terrainSliceIndex,
    };

    std::uint16_t residentIndex = m_cache.find(leafId, hint).value_or(kUnavailable);
    if (residentIndex != kUnavailable)
    {
        FoliageResidentPageEntry& entry = m_residentEntries[residentIndex];
        m_cache.touch(residentIndex);
        m_terrainSources[residentIndex] = terrainSource;
        const GenerationJobHandle activeJob = m_cache.activeJob(residentIndex);
        if (activeJob.valid() && m_generationJobs.contains(activeJob))
            m_generationJobs.job(activeJob).terrainSource = terrainSource;
        const bool ready = m_cache.isReady(residentIndex) &&
            residentHasFlag(entry, ReadyMask) &&
            residentHasFlag(entry, MaskValidMask) &&
            !residentHasFlag(entry, MaskPendingMask) &&
            !residentHasFlag(entry, UploadPendingMask);
        return ready ? residentIndex : kUnavailable;
    }

    const auto candidate = m_cache.findAllocationCandidate();
    if (!candidate) return kUnavailable;
    const auto job = m_generationJobs.tryPush({ leafId, *candidate, terrainSource });
    if (!job) return kUnavailable;

    residentIndex = *candidate;
    if (m_cache.isOpen(residentIndex))
    {
        const GenerationJobHandle oldJob = m_cache.activeJob(residentIndex);
        if (oldJob.valid() && m_generationJobs.contains(oldJob)) m_generationJobs.discard(oldJob);
        clearResidentPage(residentIndex);
    }
    else
    {
        ++m_residentCount;
    }
    m_cache.assign(residentIndex, leafId);
    m_cache.setActiveJob(residentIndex, *job);
    assignResidentPage(residentIndex, leafId);
    m_terrainSources[residentIndex] = terrainSource;
    setResidentFlag(m_residentEntries[residentIndex], MaskPendingMask, true);
    setResidentFlag(m_residentEntries[residentIndex], MaskValidMask, true);
    setResidentFlag(m_residentEntries[residentIndex], UploadPendingMask, true);
    return kUnavailable;
}

void WorldGridFoliageManager::scheduleQueuedGenerations(QuadtreeMeshRenderer& meshRenderer)
{
    HELLO_PROFILE_SCOPE("WorldGridFoliageManager::ScheduleQueuedGenerations");

    std::uint16_t dispatched = 0;
    m_generationJobs.forEachActive([&](GenerationJobHandle handle) {
        if (dispatched >= FoliageConfig::kGenerationBudgetPerFrame || m_generationJobs.isCompleted(handle) ||
            m_generationJobs.isDiscarded(handle) || m_generationJobs.isSubmitted(handle)) return;
        const GenerationJob job = m_generationJobs.job(handle);
        if (!m_cache.isOpen(job.targetSlot) || m_cache.assetId(job.targetSlot) != job.assetId ||
            m_cache.activeJob(job.targetSlot) != handle)
        {
            m_generationJobs.discard(handle);
            return;
        }
        if (!queueGpuPageGeneration(meshRenderer, job.targetSlot)) return;
        ++dispatched;
    });
    m_generationJobs.retireCompletedFront();
}

void WorldGridFoliageManager::markSubmitted(
    GenerationJobHandle job,
    const std::shared_ptr<SubmittedGpuFence>& fence)
{
    if (m_generationJobs.contains(job)) m_generationJobs.markSubmitted(job, fence);
}

void WorldGridFoliageManager::applyGeneratedPageLiveCount(
    const WorldGridQuadtreeLeafId& leafId,
    std::uint16_t pageIndex,
    std::uint16_t liveCount,
    GenerationJobHandle job)
{
    if (!m_generationJobs.contains(job) || !m_generationJobs.isSubmitted(job) ||
        !m_generationJobs.fence(job) || !m_generationJobs.fence(job)->isSignaled()) return;
    const std::uint16_t residentIndex = findResidentIndex(leafId);
    if (!m_generationJobs.isDiscarded(job) && residentIndex == pageIndex &&
        residentIndex != kUnavailable && m_cache.activeJob(residentIndex) == job)
    {
        FoliageResidentPageEntry& entry = m_residentEntries[residentIndex];
        entry.liveCount = liveCount;
        entry.contentVersion = m_nextContentVersion++;
        setResidentFlag(entry, MaskPendingMask, false);
        setResidentFlag(entry, UploadPendingMask, false);
        setResidentFlag(entry, ReadyMask, residentHasFlag(entry, MaskValidMask));
        m_cache.markReady(residentIndex);
        m_cache.clearActiveJob(residentIndex, job);
    }
    m_generationJobs.markCompleted(job);
    m_generationJobs.retireCompletedFront();
}

bool WorldGridFoliageManager::buildReadyPageInfo(
    const WorldGridQuadtreeLeafId& leafId,
    std::uint16_t residentIndex,
    FoliageReadyPageInfo& pageInfo) const
{
    if (residentIndex >= kCapacity || !m_cache.isOpen(residentIndex))
    {
        return false;
    }

    const FoliageResidentPageEntry& entry = m_residentEntries[residentIndex];
    if (entry.leafId != leafId ||
        !residentHasFlag(entry, ReadyMask) ||
        !residentHasFlag(entry, MaskValidMask) ||
        residentHasFlag(entry, MaskPendingMask) ||
        residentHasFlag(entry, UploadPendingMask))
    {
        return false;
    }

    pageInfo = {
        .pageIndex = entry.pageIndex,
        .liveCount = entry.liveCount,
        .contentVersion = entry.contentVersion,
        .seed = static_cast<std::uint32_t>(hashLeafId(entry.leafId)),
    };
    return true;
}

CacheIndex WorldGridFoliageManager::isResident(
    const WorldGridQuadtreeLeafId& leafId, CacheIndex hint) const
{
    const auto slot = m_cache.isResident(leafId, hint);
    if (slot == kUnavailable) return kUnavailable;
    const auto& entry = m_residentEntries[slot];
    return residentHasFlag(entry, ReadyMask) &&
        residentHasFlag(entry, MaskValidMask) &&
        !residentHasFlag(entry, MaskPendingMask) &&
        !residentHasFlag(entry, UploadPendingMask) ? slot : kUnavailable;
}

bool WorldGridFoliageManager::emitPageDraw(
    const WorldGridQuadtreeLeafId& pageId,
    std::uint16_t residentIndex,
    const WorldGridQuadtreeLeafId& terrainLeafId,
    std::uint16_t terrainSliceIndex,
    FoliageImposterRenderer& foliageRenderer) const
{
    if (residentIndex >= kCapacity)
    {
        return false;
    }

    const FoliageResidentPageEntry& entry = m_residentEntries[residentIndex];
    const auto [terrainLeafOrigin, terrainLeafMaxCorner] = worldGridQuadtreeLeafBounds(terrainLeafId);
    (void)terrainLeafMaxCorner;
    const auto [pageOrigin, pageMaxCorner] = worldGridQuadtreeLeafBounds(pageId);
    (void)pageMaxCorner;
    foliageRenderer.addPageDraw({
        .pageIndex = entry.pageIndex,
        .liveCount = entry.liveCount,
        .seed = static_cast<std::uint32_t>(hashLeafId(entry.leafId)),
        .pageOrigin = pageOrigin,
        .terrainLeafOrigin = terrainLeafOrigin,
        .terrainSliceIndex = terrainSliceIndex,
        .terrainScalePow = worldGridQuadtreeLeafScalePow(terrainLeafId),
    });
    return true;
}

std::uint16_t WorldGridFoliageManager::maskPendingCount() const
{
    std::uint16_t count = 0;
    for (std::uint16_t residentIndex = 0; residentIndex < kCapacity; ++residentIndex)
    {
        if (m_cache.isOpen(residentIndex) &&
            residentHasFlag(m_residentEntries[residentIndex], MaskPendingMask))
        {
            ++count;
        }
    }
    return count;
}

std::uint16_t WorldGridFoliageManager::uploadPendingCount() const
{
    std::uint16_t count = 0;
    for (std::uint16_t residentIndex = 0; residentIndex < kCapacity; ++residentIndex)
    {
        if (m_cache.isOpen(residentIndex) &&
            residentHasFlag(m_residentEntries[residentIndex], UploadPendingMask))
        {
            ++count;
        }
    }
    return count;
}

std::uint16_t WorldGridFoliageManager::readyCount() const
{
    std::uint16_t count = 0;
    for (std::uint16_t residentIndex = 0; residentIndex < kCapacity; ++residentIndex)
    {
        if (!m_cache.isOpen(residentIndex))
        {
            continue;
        }

        const FoliageResidentPageEntry& entry = m_residentEntries[residentIndex];
        if (residentHasFlag(entry, ReadyMask) &&
            residentHasFlag(entry, MaskValidMask) &&
            !residentHasFlag(entry, MaskPendingMask) &&
            !residentHasFlag(entry, UploadPendingMask))
        {
            ++count;
        }
    }
    return count;
}

std::uint16_t WorldGridFoliageManager::findResidentIndex(const WorldGridQuadtreeLeafId& leafId) const
{
    return m_cache.find(leafId).value_or(kUnavailable);
}


std::uint64_t WorldGridFoliageManager::mix64(std::uint64_t x)
{
    x ^= x >> 30U;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27U;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31U;
    return x;
}

std::uint64_t WorldGridFoliageManager::hashLeafId(const WorldGridQuadtreeLeafId& leafId)
{
    const std::uint64_t word0 = std::bit_cast<std::uint64_t>(leafId.gridX);
    const std::uint64_t word1 = std::bit_cast<std::uint64_t>(leafId.gridY);
    const std::uint64_t word2 = leafId.subdivisionPath;

    std::uint64_t hash = 0x9e3779b97f4a7c15ULL;
    hash ^= mix64(word0 + 0x9e3779b97f4a7c15ULL);
    hash = mix64(hash);
    hash ^= mix64(word1 + 0xbf58476d1ce4e5b9ULL);
    hash = mix64(hash);
    hash ^= mix64(word2 + 0x94d049bb133111ebULL);
    hash = mix64(hash);
    return hash;
}


std::size_t WorldGridFoliageManager::LeafIdHash::operator()(const WorldGridQuadtreeLeafId& leafId) const
{
    return static_cast<std::size_t>(WorldGridFoliageManager::hashLeafId(leafId));
}

void WorldGridFoliageManager::assignResidentPage(std::uint16_t residentIndex, const WorldGridQuadtreeLeafId& leafId)
{
    m_residentEntries[residentIndex] = {
        .leafId = leafId,
        .pageIndex = residentIndex,
        .liveCount = 0,
        .contentVersion = 0u,
        .flags = 0,
    };
    m_terrainSources[residentIndex] = {};
}

void WorldGridFoliageManager::clearResidentPage(std::uint16_t residentIndex)
{
    m_residentEntries[residentIndex] = {};
    m_terrainSources[residentIndex] = {};
}

void WorldGridFoliageManager::resetCacheState()
{
    m_cache.clear();
    m_generationJobs.discardAll();
    m_residentEntries.fill({});
    m_terrainSources.fill({});
    m_residentCount = 0;
    m_nextContentVersion = 1u;
}

bool WorldGridFoliageManager::queueGpuPageGeneration(
    QuadtreeMeshRenderer& meshRenderer,
    std::uint16_t residentIndex)
{
    const FoliageResidentPageEntry& entry = m_residentEntries[residentIndex];
    const FoliageTerrainSource& terrainSource = m_terrainSources[residentIndex];
    return meshRenderer.queueFoliagePageGeneration(
        entry.leafId,
        terrainSource.terrainLeafId,
        terrainSource.terrainSliceIndex,
        entry.pageIndex,
        m_waterLevel,
        m_cache.activeJob(residentIndex));
}

bool WorldGridFoliageManager::residentHasFlag(const FoliageResidentPageEntry& entry, std::uint8_t mask)
{
    return (entry.flags & mask) != 0u;
}

void WorldGridFoliageManager::setResidentFlag(FoliageResidentPageEntry& entry, std::uint8_t mask, bool enabled)
{
    if (enabled)
    {
        entry.flags |= mask;
    }
    else
    {
        entry.flags &= static_cast<std::uint8_t>(~mask);
    }
}
