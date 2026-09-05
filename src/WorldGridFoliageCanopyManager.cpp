#include "WorldGridFoliageCanopyManager.hpp"

#include "FoliageCanopyRenderer.hpp"
#include "PerformanceCapture.hpp"

#include <algorithm>
#include <bit>
#include <limits>

WorldGridFoliageCanopyManager::WorldGridFoliageCanopyManager()
    : m_cache(kCapacity, FoliageConfig::kCanopyLookupBucketCount, FoliageConfig::kCanopyLookupBucketEntryCount)
    , m_generationJobs(kCapacity)
{
    resetCacheState();
}

void WorldGridFoliageCanopyManager::ageMap()
{
    m_generationJobs.processSignaled([&](GenerationJobHandle handle, const GenerationJob& job) {
        if (m_cache.isOpen(job.targetSlot) && m_cache.assetId(job.targetSlot) == job.assetId &&
            m_cache.activeJob(job.targetSlot) == handle)
        {
            setResidentFlag(m_residentEntries[job.targetSlot], ReadyMask, true);
            m_cache.markReady(job.targetSlot);
            m_cache.clearActiveJob(job.targetSlot, handle);
        }
    });
    ++m_requestFrame;
    m_cache.age();

    for (std::uint16_t residentIndex = 0; residentIndex < kCapacity; ++residentIndex)
    {
        if (!m_cache.isOpen(residentIndex)) continue;
        FoliageCanopyResidentCellEntry& entry = m_residentEntries[residentIndex];
        if (entry.residentFrameAge < std::numeric_limits<std::uint8_t>::max())
        {
            ++entry.residentFrameAge;
        }
    }
}

void WorldGridFoliageCanopyManager::setWaterLevel(float waterLevel)
{
    if (m_waterLevel != waterLevel)
    {
        m_waterLevel = waterLevel;
        clearCache();
    }
}

void WorldGridFoliageCanopyManager::clearCache()
{
    resetCacheState();
}

void WorldGridFoliageCanopyManager::shutdownAfterGpuIdle()
{
    m_cache.clear();
    m_generationJobs.clear();
}

std::uint16_t WorldGridFoliageCanopyManager::requestAsset(
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
        FoliageCanopyResidentCellEntry& entry = m_residentEntries[residentIndex];
        m_cache.touch(residentIndex);
        m_terrainSources[residentIndex] = terrainSource;
        const GenerationJobHandle activeJob = m_cache.activeJob(residentIndex);
        if (activeJob.valid() && m_generationJobs.contains(activeJob))
        {
            m_generationJobs.job(activeJob).terrainSource = terrainSource;
            m_generationJobs.job(activeJob).requestFrame = m_requestFrame;
        }
        return residentHasFlag(entry, ReadyMask) ? residentIndex : kUnavailable;
    }

    const auto candidate = m_cache.findAllocationCandidate();
    if (!candidate) return kUnavailable;
    const auto job = m_generationJobs.tryPush({ leafId, *candidate, terrainSource, m_requestFrame });
    if (!job) return kUnavailable;
    residentIndex = *candidate;
    const bool wasOpen = m_cache.isOpen(residentIndex);
    if (wasOpen)
    {
        const GenerationJobHandle oldJob = m_cache.activeJob(residentIndex);
        if (oldJob.valid() && m_generationJobs.contains(oldJob)) m_generationJobs.discard(oldJob);
        clearResidentCell(residentIndex, false);
    }
    m_cache.assign(residentIndex, leafId);
    m_cache.setActiveJob(residentIndex, *job);
    assignResidentCell(residentIndex, leafId);
    m_terrainSources[residentIndex] = terrainSource;
    if (!wasOpen) ++m_residentCount;
    return kUnavailable;
}

void WorldGridFoliageCanopyManager::scheduleQueuedGenerations(FoliageCanopyRenderer& renderer)
{
    HELLO_PROFILE_SCOPE("WorldGridFoliageCanopyManager::ScheduleQueuedGenerations");

    std::uint16_t dispatched = 0;
    m_generationJobs.forEachActive([&](GenerationJobHandle handle) {
        if (dispatched >= FoliageConfig::kCanopyGenerationBudgetPerFrame || m_generationJobs.isCompleted(handle) ||
            m_generationJobs.isDiscarded(handle) || m_generationJobs.isSubmitted(handle)) return;
        const GenerationJob job = m_generationJobs.job(handle);
        if (job.requestFrame != m_requestFrame)
        {
            m_generationJobs.discard(handle);
            if (m_cache.isOpen(job.targetSlot) && m_cache.activeJob(job.targetSlot) == handle)
            {
                m_cache.clearActiveJob(job.targetSlot, handle);
                m_cache.release(job.targetSlot);
                clearResidentCell(job.targetSlot);
            }
            return;
        }
        if (!m_cache.isOpen(job.targetSlot) || m_cache.assetId(job.targetSlot) != job.assetId || m_cache.activeJob(job.targetSlot) != handle)
        { m_generationJobs.discard(handle); return; }
        if (!renderer.queueCellGeneration(
                job.assetId,
                job.terrainSource.terrainLeafId,
                job.terrainSource.terrainSliceIndex,
                job.targetSlot,
                m_waterLevel,
                handle))
        {
            return;
        }
        ++dispatched;
    });
    m_generationJobs.retireCompletedFront();
}

void WorldGridFoliageCanopyManager::markSubmitted(
    GenerationJobHandle job,
    const std::shared_ptr<SubmittedGpuFence>& fence)
{
    if (m_generationJobs.contains(job)) m_generationJobs.markSubmitted(job, fence);
}

bool WorldGridFoliageCanopyManager::buildReadyCellInfo(
    const WorldGridQuadtreeLeafId& leafId,
    std::uint16_t residentIndex,
    FoliageCanopyReadyCellInfo& cellInfo) const
{
    if (residentIndex >= kCapacity || !m_cache.isOpen(residentIndex))
    {
        return false;
    }

    const FoliageCanopyResidentCellEntry& entry = m_residentEntries[residentIndex];
    if (entry.leafId != leafId || !residentHasFlag(entry, ReadyMask))
    {
        return false;
    }

    cellInfo = {
        .slotIndex = entry.slotIndex,
        .seed = static_cast<std::uint32_t>(hashLeafId(leafId)),
        .residentFrameAge = entry.residentFrameAge,
    };
    return true;
}

CacheIndex WorldGridFoliageCanopyManager::isResident(
    const WorldGridQuadtreeLeafId& leafId, CacheIndex hint) const
{
    const auto slot = m_cache.isResident(leafId, hint);
    if (slot == kUnavailable) return kUnavailable;
    const auto& entry = m_residentEntries[slot];
    return residentHasFlag(entry, ReadyMask) ? slot : kUnavailable;
}

void WorldGridFoliageCanopyManager::emitCanopyDraw(
    const WorldGridQuadtreeLeafId& nodeId,
    std::uint16_t terrainSliceIndex,
    const WorldGridQuadtreeLeafId* cellIds,
    const std::array<std::uint16_t, FoliageConfig::kCanopyCellCountPerNode>& residentIndices,
    std::uint32_t cellCount,
    std::uint32_t readyCellCount,
    std::uint8_t drawAgeFrames,
    const std::array<std::uint8_t, 4>& edgeFadeStrengths,
    FoliageCanopyRenderer& renderer) const
{
    FoliageCanopyDrawReference drawReference{};
    drawReference.patchOrigin = worldGridQuadtreeLeafBounds(nodeId).first;
    drawReference.terrainLeafOrigin = drawReference.patchOrigin;
    drawReference.patchSizeMeters = static_cast<float>(worldGridQuadtreeLeafSize(nodeId));
    drawReference.terrainLeafSizeMeters = drawReference.patchSizeMeters;
    drawReference.terrainSliceIndex = terrainSliceIndex;
    drawReference.patchSeed = static_cast<std::uint32_t>(hashLeafId(nodeId));
    drawReference.drawAgeFrames = drawAgeFrames;
    drawReference.edgeFadeStrengths = edgeFadeStrengths;
    drawReference.cellSlotIndices.fill(UINT16_MAX);
    drawReference.cellSeeds.fill(0u);

    for (std::uint32_t cellIndex = 0; cellIndex < cellCount; ++cellIndex)
    {
        if (residentIndices[cellIndex] == kUnavailable)
        {
            continue;
        }

        const FoliageCanopyResidentCellEntry& entry = m_residentEntries[residentIndices[cellIndex]];
        drawReference.cellSlotIndices[cellIndex] = entry.slotIndex;
        drawReference.cellSeeds[cellIndex] = static_cast<std::uint32_t>(hashLeafId(entry.leafId));
    }

    renderer.addCanopyDraw(drawReference);
}

void WorldGridFoliageCanopyManager::noteRenderedCell(const WorldGridQuadtreeLeafId& leafId)
{
    const std::uint16_t residentIndex = findResidentIndex(leafId);
    if (residentIndex == kUnavailable)
    {
        return;
    }

    m_cache.touch(residentIndex);
}

std::uint16_t WorldGridFoliageCanopyManager::readyCount() const
{
    std::uint16_t count = 0;
    for (std::uint16_t residentIndex = 0; residentIndex < kCapacity; ++residentIndex)
    {
        if (m_cache.isOpen(residentIndex) && residentHasFlag(m_residentEntries[residentIndex], ReadyMask))
        {
            ++count;
        }
    }
    return count;
}

std::uint16_t WorldGridFoliageCanopyManager::findResidentIndex(const WorldGridQuadtreeLeafId& leafId) const
{
    return m_cache.find(leafId).value_or(kUnavailable);
}


std::uint64_t WorldGridFoliageCanopyManager::mix64(std::uint64_t x)
{
    x ^= x >> 30U;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27U;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31U;
    return x;
}

std::uint64_t WorldGridFoliageCanopyManager::hashLeafId(const WorldGridQuadtreeLeafId& leafId)
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


std::size_t WorldGridFoliageCanopyManager::LeafIdHash::operator()(const WorldGridQuadtreeLeafId& leafId) const
{
    return static_cast<std::size_t>(WorldGridFoliageCanopyManager::hashLeafId(leafId));
}

void WorldGridFoliageCanopyManager::assignResidentCell(std::uint16_t residentIndex, const WorldGridQuadtreeLeafId& leafId)
{
    m_residentEntries[residentIndex] = {
        .leafId = leafId,
        .slotIndex = residentIndex,
        .residentFrameAge = 0,
        .flags = 0,
    };
    setResidentFlag(m_residentEntries[residentIndex], ReadyMask, false);
    m_terrainSources[residentIndex] = {};
}

void WorldGridFoliageCanopyManager::clearResidentCell(std::uint16_t residentIndex, bool removeFromActiveSet)
{
    if (removeFromActiveSet && m_residentCount > 0) --m_residentCount;

    m_residentEntries[residentIndex] = {};
    m_terrainSources[residentIndex] = {};
}

void WorldGridFoliageCanopyManager::resetCacheState()
{
    m_cache.clear();
    m_generationJobs.discardAll();
    m_residentEntries.fill({});
    m_terrainSources.fill({});
    m_residentCount = 0;
    m_requestFrame = 0;
}

bool WorldGridFoliageCanopyManager::residentHasFlag(const FoliageCanopyResidentCellEntry& entry, std::uint8_t mask)
{
    return (entry.flags & mask) != 0u;
}

void WorldGridFoliageCanopyManager::setResidentFlag(
    FoliageCanopyResidentCellEntry& entry,
    std::uint8_t mask,
    bool enabled)
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
