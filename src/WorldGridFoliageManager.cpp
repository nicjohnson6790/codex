#include "WorldGridFoliageManager.hpp"

#include "PerformanceCapture.hpp"
#include "QuadtreeMeshRenderer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace
{
bool terrainLayerSettingsEqual(
    const TerrainFractalNoiseLayerSettings& a,
    const TerrainFractalNoiseLayerSettings& b)
{
    return
        a.wavelength == b.wavelength &&
        a.amplitude == b.amplitude &&
        a.bias == b.bias &&
        a.initialFrequency == b.initialFrequency &&
        a.initialAmplitude == b.initialAmplitude &&
        a.octaveCount == b.octaveCount &&
        a.octaveFrequencyScale == b.octaveFrequencyScale &&
        a.octaveAmplitudeScale == b.octaveAmplitudeScale &&
        a.gradientDampenStrength == b.gradientDampenStrength &&
        a.octaveRotationDegrees == b.octaveRotationDegrees;
}

bool terrainBlendSettingsEqual(
    const TerrainBlendNoiseSettings& a,
    const TerrainBlendNoiseSettings& b)
{
    return
        a.wavelength == b.wavelength &&
        a.initialFrequency == b.initialFrequency &&
        a.initialAmplitude == b.initialAmplitude &&
        a.octaveCount == b.octaveCount &&
        a.octaveFrequencyScale == b.octaveFrequencyScale &&
        a.octaveAmplitudeScale == b.octaveAmplitudeScale &&
        a.gradientDampenStrength == b.gradientDampenStrength &&
        a.octaveRotationDegrees == b.octaveRotationDegrees &&
        a.lowThreshold == b.lowThreshold &&
        a.highThreshold == b.highThreshold &&
        a.lowTransitionWidth == b.lowTransitionWidth &&
        a.highTransitionWidth == b.highTransitionWidth;
}

bool terrainNoiseSettingsEqual(const TerrainNoiseSettings& a, const TerrainNoiseSettings& b)
{
    return
        a.baseHeight == b.baseHeight &&
        terrainLayerSettingsEqual(a.hills, b.hills) &&
        terrainLayerSettingsEqual(a.mediumDetail, b.mediumDetail) &&
        terrainLayerSettingsEqual(a.highDetail, b.highDetail) &&
        terrainBlendSettingsEqual(a.blend, b.blend);
}

}

WorldGridFoliageManager::WorldGridFoliageManager()
{
    resetCacheState();
}

void WorldGridFoliageManager::ageMap()
{
    for (std::uint16_t residentIndex = 0; residentIndex < kCapacity; ++residentIndex)
    {
        if (!m_residentUsed[residentIndex])
        {
            continue;
        }

        FoliageResidentPageEntry& entry = m_residentEntries[residentIndex];
        if (entry.age < std::numeric_limits<std::uint8_t>::max())
        {
            ++entry.age;
        }
    }
}

void WorldGridFoliageManager::setTerrainSettings(const TerrainNoiseSettings& settings)
{
    const TerrainNoiseSettings sanitized = sanitizeTerrainNoiseSettings(settings);
    if (!terrainNoiseSettingsEqual(m_terrainSettings, sanitized))
    {
        m_terrainSettings = sanitized;
        clearCache();
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

bool WorldGridFoliageManager::makeResident(
    const WorldGridQuadtreeLeafId& leafId,
    const WorldGridQuadtreeLeafId& terrainLeafId,
    std::uint16_t terrainSliceIndex)
{
    const FoliageTerrainSource terrainSource{
        .terrainLeafId = terrainLeafId,
        .terrainSliceIndex = terrainSliceIndex,
    };

    const std::uint16_t residentIndex = findResidentIndex(leafId);
    if (residentIndex != kCapacity)
    {
        FoliageResidentPageEntry& entry = m_residentEntries[residentIndex];
        entry.age = 0;
        m_terrainSources[residentIndex] = terrainSource;
        return
            residentHasFlag(entry, ReadyMask) &&
            residentHasFlag(entry, MaskValidMask) &&
            !residentHasFlag(entry, MaskPendingMask) &&
            !residentHasFlag(entry, UploadPendingMask);
    }

    enqueueLeaf(leafId, terrainSource);
    return false;
}

void WorldGridFoliageManager::scheduleQueuedGenerations(QuadtreeMeshRenderer& meshRenderer)
{
    HELLO_PROFILE_SCOPE("WorldGridFoliageManager::ScheduleQueuedGenerations");

    for (std::uint16_t dispatchIndex = 0; dispatchIndex < FoliageConfig::kGenerationBudgetPerFrame; ++dispatchIndex)
    {
        QueuedLeafRequest request{};
        if (!dequeueLeaf(request))
        {
            return;
        }

        std::uint16_t residentIndex = findResidentIndex(request.leafId);
        if (residentIndex != kCapacity)
        {
            m_residentEntries[residentIndex].age = 0;
            m_terrainSources[residentIndex] = request.terrainSource;
            continue;
        }

        bool usesFreeSlot = false;
        if (m_freeResidentIndexCount > 0)
        {
            residentIndex = m_freeResidentIndices[--m_freeResidentIndexCount];
            usesFreeSlot = true;
        }
        else
        {
            residentIndex = findOldestEvictableResidentIndex();
            if (residentIndex == kCapacity)
            {
                enqueueLeaf(request.leafId, request.terrainSource);
                return;
            }
        }

        if (!usesFreeSlot)
        {
            removeResidentLookup(m_residentEntries[residentIndex].leafId, residentIndex);
            clearResidentPage(residentIndex);
        }

        assignResidentPage(residentIndex, request.leafId);
        m_terrainSources[residentIndex] = request.terrainSource;
        setResidentFlag(m_residentEntries[residentIndex], MaskPendingMask, true);
        setResidentFlag(m_residentEntries[residentIndex], MaskValidMask, true);
        insertResidentLookup(request.leafId, residentIndex);
        if (usesFreeSlot)
        {
            ++m_residentCount;
        }

        setResidentFlag(m_residentEntries[residentIndex], UploadPendingMask, true);
        setResidentFlag(m_residentEntries[residentIndex], ReadyMask, false);

        if (!queueGpuPageGeneration(meshRenderer, residentIndex))
        {
            removeResidentLookup(request.leafId, residentIndex);
            clearResidentPage(residentIndex);
            if (usesFreeSlot)
            {
                --m_residentCount;
            }
            m_freeResidentIndices[m_freeResidentIndexCount++] = residentIndex;
            enqueueLeaf(request.leafId, request.terrainSource);
            return;
        }
    }
}

void WorldGridFoliageManager::applyGeneratedPageLiveCounts(
    const std::vector<std::pair<WorldGridQuadtreeLeafId, std::uint16_t>>& generatedLiveCounts)
{
    for (const auto& [leafId, liveCount] : generatedLiveCounts)
    {
        const std::uint16_t residentIndex = findResidentIndex(leafId);
        if (residentIndex == kCapacity)
        {
            continue;
        }

        FoliageResidentPageEntry& entry = m_residentEntries[residentIndex];
        entry.liveCount = liveCount;
        entry.contentVersion = m_nextContentVersion++;
        setResidentFlag(entry, MaskPendingMask, false);
        setResidentFlag(entry, UploadPendingMask, false);
        setResidentFlag(entry, ReadyMask, residentHasFlag(entry, MaskValidMask));
    }
}

bool WorldGridFoliageManager::getReadyPageInfo(
    const WorldGridQuadtreeLeafId& leafId,
    FoliageReadyPageInfo& pageInfo) const
{
    const std::uint16_t residentIndex = findResidentIndex(leafId);
    if (residentIndex == kCapacity)
    {
        return false;
    }

    const FoliageResidentPageEntry& entry = m_residentEntries[residentIndex];
    if (!residentHasFlag(entry, ReadyMask) ||
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

std::uint16_t WorldGridFoliageManager::maskPendingCount() const
{
    std::uint16_t count = 0;
    for (std::uint16_t residentIndex = 0; residentIndex < kCapacity; ++residentIndex)
    {
        if (m_residentUsed[residentIndex] &&
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
        if (m_residentUsed[residentIndex] &&
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
        if (!m_residentUsed[residentIndex])
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
    const std::uint8_t bucketIndex = bucketIndexForLeafId(leafId);
    const auto& bucket = m_lookupBuckets[bucketIndex];

    for (const LookupBucketEntry& bucketEntry : bucket)
    {
        if (bucketEntry.residentIndex == kCapacity)
        {
            continue;
        }

        const std::uint16_t residentIndex = bucketEntry.residentIndex;
        if (m_residentUsed[residentIndex] && m_residentEntries[residentIndex].leafId == leafId)
        {
            return residentIndex;
        }
    }

    if (!m_lookupBucketHasOverflow[bucketIndex])
    {
        return kCapacity;
    }

    for (const LookupOverflowEntry& overflowEntry : m_lookupOverflowEntries)
    {
        if (!overflowEntry.used || overflowEntry.bucketIndex != bucketIndex)
        {
            continue;
        }

        if (overflowEntry.leafId == leafId)
        {
            return overflowEntry.residentIndex;
        }
    }

    return kCapacity;
}

void WorldGridFoliageManager::insertResidentLookup(const WorldGridQuadtreeLeafId& leafId, std::uint16_t residentIndex)
{
    const std::uint8_t bucketIndex = bucketIndexForLeafId(leafId);
    auto& bucket = m_lookupBuckets[bucketIndex];

    for (LookupBucketEntry& bucketEntry : bucket)
    {
        if (bucketEntry.residentIndex == kCapacity)
        {
            bucketEntry.residentIndex = residentIndex;
            return;
        }
    }

    for (LookupOverflowEntry& overflowEntry : m_lookupOverflowEntries)
    {
        if (overflowEntry.used)
        {
            continue;
        }

        overflowEntry = {
            .leafId = leafId,
            .residentIndex = residentIndex,
            .bucketIndex = bucketIndex,
            .used = true,
        };
        m_lookupBucketHasOverflow[bucketIndex] = true;
        ++m_lookupOverflowCount;
        return;
    }
}

void WorldGridFoliageManager::removeResidentLookup(const WorldGridQuadtreeLeafId& leafId, std::uint16_t residentIndex)
{
    const std::uint8_t bucketIndex = bucketIndexForLeafId(leafId);
    auto& bucket = m_lookupBuckets[bucketIndex];

    for (LookupBucketEntry& bucketEntry : bucket)
    {
        if (bucketEntry.residentIndex != residentIndex)
        {
            continue;
        }

        bucketEntry.residentIndex = kCapacity;
        if (!m_lookupBucketHasOverflow[bucketIndex])
        {
            return;
        }

        for (LookupOverflowEntry& overflowEntry : m_lookupOverflowEntries)
        {
            if (!overflowEntry.used || overflowEntry.bucketIndex != bucketIndex)
            {
                continue;
            }

            bucketEntry.residentIndex = overflowEntry.residentIndex;
            overflowEntry = {};
            --m_lookupOverflowCount;

            bool hasOverflow = false;
            for (const LookupOverflowEntry& remainingEntry : m_lookupOverflowEntries)
            {
                if (remainingEntry.used && remainingEntry.bucketIndex == bucketIndex)
                {
                    hasOverflow = true;
                    break;
                }
            }
            m_lookupBucketHasOverflow[bucketIndex] = hasOverflow;
            return;
        }

        m_lookupBucketHasOverflow[bucketIndex] = false;
        return;
    }

    for (LookupOverflowEntry& overflowEntry : m_lookupOverflowEntries)
    {
        if (!overflowEntry.used || overflowEntry.bucketIndex != bucketIndex || overflowEntry.residentIndex != residentIndex)
        {
            continue;
        }

        overflowEntry = {};
        --m_lookupOverflowCount;

        bool hasOverflow = false;
        for (const LookupOverflowEntry& remainingEntry : m_lookupOverflowEntries)
        {
            if (remainingEntry.used && remainingEntry.bucketIndex == bucketIndex)
            {
                hasOverflow = true;
                break;
            }
        }
        m_lookupBucketHasOverflow[bucketIndex] = hasOverflow;
        return;
    }
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

std::uint8_t WorldGridFoliageManager::bucketIndexForLeafId(const WorldGridQuadtreeLeafId& leafId)
{
    return static_cast<std::uint8_t>(hashLeafId(leafId) >> 56U);
}

bool WorldGridFoliageManager::queueContains(const WorldGridQuadtreeLeafId& leafId) const
{
    for (std::uint16_t offset = 0; offset < m_queueCount; ++offset)
    {
        const std::uint16_t queueIndex = static_cast<std::uint16_t>((m_queueStart + offset) % kCapacity);
        if (m_leafQueue[queueIndex].leafId == leafId)
        {
            return true;
        }
    }

    return false;
}

bool WorldGridFoliageManager::enqueueLeaf(const WorldGridQuadtreeLeafId& leafId, const FoliageTerrainSource& terrainSource)
{
    for (std::uint16_t offset = 0; offset < m_queueCount; ++offset)
    {
        const std::uint16_t queueIndex = static_cast<std::uint16_t>((m_queueStart + offset) % kCapacity);
        if (m_leafQueue[queueIndex].leafId == leafId)
        {
            m_leafQueue[queueIndex].terrainSource = terrainSource;
            return true;
        }
    }

    if (m_queueCount >= kCapacity)
    {
        return false;
    }

    m_leafQueue[m_queueEnd] = {
        .leafId = leafId,
        .terrainSource = terrainSource,
    };
    m_queueEnd = static_cast<std::uint16_t>((m_queueEnd + 1u) % kCapacity);
    ++m_queueCount;
    return true;
}

bool WorldGridFoliageManager::dequeueLeaf(QueuedLeafRequest& request)
{
    if (m_queueCount == 0)
    {
        return false;
    }

    request = m_leafQueue[m_queueStart];
    m_queueStart = static_cast<std::uint16_t>((m_queueStart + 1u) % kCapacity);
    --m_queueCount;
    return true;
}

std::uint16_t WorldGridFoliageManager::findOldestEvictableResidentIndex() const
{
    std::uint16_t oldestResidentIndex = kCapacity;
    std::uint8_t oldestAge = 0;

    for (std::uint16_t residentIndex = 0; residentIndex < kCapacity; ++residentIndex)
    {
        if (!m_residentUsed[residentIndex])
        {
            continue;
        }

        const FoliageResidentPageEntry& entry = m_residentEntries[residentIndex];
        if (residentHasFlag(entry, UploadPendingMask) ||
            residentHasFlag(entry, MaskPendingMask) ||
            entry.age == 0)
        {
            continue;
        }

        if (oldestResidentIndex == kCapacity || entry.age > oldestAge)
        {
            oldestResidentIndex = residentIndex;
            oldestAge = entry.age;
        }
    }

    return oldestResidentIndex;
}

void WorldGridFoliageManager::assignResidentPage(std::uint16_t residentIndex, const WorldGridQuadtreeLeafId& leafId)
{
    m_residentUsed[residentIndex] = true;
    m_residentEntries[residentIndex] = {
        .leafId = leafId,
        .pageIndex = residentIndex,
        .liveCount = 0,
        .contentVersion = 0u,
        .age = 0,
        .flags = 0,
    };
    m_terrainSources[residentIndex] = {};
}

void WorldGridFoliageManager::clearResidentPage(std::uint16_t residentIndex)
{
    m_residentUsed[residentIndex] = false;
    m_residentEntries[residentIndex] = {};
    m_terrainSources[residentIndex] = {};
}

void WorldGridFoliageManager::resetCacheState()
{
    m_queueStart = 0;
    m_queueEnd = 0;
    m_queueCount = 0;
    m_leafQueue.fill({});
    m_residentEntries.fill({});
    m_residentUsed.fill(false);
    m_terrainSources.fill({});
    for (auto& bucket : m_lookupBuckets)
    {
        bucket.fill({});
    }
    m_lookupBucketHasOverflow.fill(false);
    m_lookupOverflowEntries.fill({});
    for (std::uint16_t index = 0; index < kCapacity; ++index)
    {
        m_freeResidentIndices[index] = static_cast<std::uint16_t>(kCapacity - 1u - index);
    }
    m_residentCount = 0;
    m_lookupOverflowCount = 0;
    m_freeResidentIndexCount = kCapacity;
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
        m_waterLevel);
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
