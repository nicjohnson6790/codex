#include "WorldGridFoliageCanopyManager.hpp"

#include "FoliageCanopyRenderer.hpp"
#include "PerformanceCapture.hpp"

#include <algorithm>
#include <bit>
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

WorldGridFoliageCanopyManager::WorldGridFoliageCanopyManager()
{
    resetCacheState();
}

void WorldGridFoliageCanopyManager::ageMap()
{
    ++m_requestFrame;

    for (std::uint16_t activeIndex = 0; activeIndex < m_residentCount; ++activeIndex)
    {
        const std::uint16_t residentIndex = m_activeResidentIndices[activeIndex];
        FoliageCanopyResidentCellEntry& entry = m_residentEntries[residentIndex];
        if (entry.evictionAge < std::numeric_limits<std::uint8_t>::max())
        {
            ++entry.evictionAge;
        }
        if (entry.residentFrameAge < std::numeric_limits<std::uint8_t>::max())
        {
            ++entry.residentFrameAge;
        }
        m_generationLocked[residentIndex] = false;
    }
}

void WorldGridFoliageCanopyManager::setTerrainSettings(const TerrainNoiseSettings& settings)
{
    const TerrainNoiseSettings sanitized = sanitizeTerrainNoiseSettings(settings);
    if (!terrainNoiseSettingsEqual(m_terrainSettings, sanitized))
    {
        m_terrainSettings = sanitized;
        clearCache();
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

bool WorldGridFoliageCanopyManager::makeResident(
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
        FoliageCanopyResidentCellEntry& entry = m_residentEntries[residentIndex];
        entry.evictionAge = 0;
        m_terrainSources[residentIndex] = terrainSource;
        return residentHasFlag(entry, ReadyMask);
    }

    (void)enqueueLeaf(leafId, terrainSource);
    return false;
}

void WorldGridFoliageCanopyManager::scheduleQueuedGenerations(FoliageCanopyRenderer& renderer)
{
    HELLO_PROFILE_SCOPE("WorldGridFoliageCanopyManager::ScheduleQueuedGenerations");

    for (std::uint16_t dispatchIndex = 0; dispatchIndex < FoliageConfig::kCanopyGenerationBudgetPerFrame; ++dispatchIndex)
    {
        QueuedLeafRequest request{};
        if (!dequeueLeaf(request))
        {
            return;
        }

        if (request.requestFrame != m_requestFrame)
        {
            --dispatchIndex;
            continue;
        }

        std::uint16_t residentIndex = findResidentIndex(request.leafId);
        if (residentIndex != kCapacity)
        {
            m_residentEntries[residentIndex].evictionAge = 0;
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
                (void)enqueueLeaf(request.leafId, request.terrainSource);
                return;
            }
        }

        if (!usesFreeSlot)
        {
            removeResidentLookup(m_residentEntries[residentIndex].leafId, residentIndex);
            clearResidentCell(residentIndex, false);
        }

        assignResidentCell(residentIndex, request.leafId);
        m_terrainSources[residentIndex] = request.terrainSource;
        insertResidentLookup(request.leafId, residentIndex);
        if (usesFreeSlot)
        {
            ++m_residentCount;
        }

        m_generationLocked[residentIndex] = true;
        if (!renderer.queueCellGeneration(
            request.leafId,
            request.terrainSource.terrainLeafId,
            request.terrainSource.terrainSliceIndex,
            m_residentEntries[residentIndex].slotIndex,
            m_waterLevel))
        {
            removeResidentLookup(request.leafId, residentIndex);
            clearResidentCell(residentIndex);
            if (usesFreeSlot)
            {
                --m_residentCount;
            }
            m_freeResidentIndices[m_freeResidentIndexCount++] = residentIndex;
            (void)enqueueLeaf(request.leafId, request.terrainSource);
            return;
        }

        setResidentFlag(m_residentEntries[residentIndex], ReadyMask, true);
    }
}

bool WorldGridFoliageCanopyManager::getReadyCellInfo(
    const WorldGridQuadtreeLeafId& leafId,
    FoliageCanopyReadyCellInfo& cellInfo) const
{
    const std::uint16_t residentIndex = findResidentIndex(leafId);
    if (residentIndex == kCapacity)
    {
        return false;
    }

    const FoliageCanopyResidentCellEntry& entry = m_residentEntries[residentIndex];
    if (!residentHasFlag(entry, ReadyMask))
    {
        return false;
    }

    cellInfo = {
        .slotIndex = entry.slotIndex,
        .seed = static_cast<std::uint32_t>(hashLeafId(entry.leafId)),
        .residentFrameAge = entry.residentFrameAge,
    };
    return true;
}

void WorldGridFoliageCanopyManager::noteRenderedCell(const WorldGridQuadtreeLeafId& leafId)
{
    const std::uint16_t residentIndex = findResidentIndex(leafId);
    if (residentIndex == kCapacity)
    {
        return;
    }

    m_residentEntries[residentIndex].evictionAge = 0;
}

std::uint16_t WorldGridFoliageCanopyManager::readyCount() const
{
    std::uint16_t count = 0;
    for (std::uint16_t activeIndex = 0; activeIndex < m_residentCount; ++activeIndex)
    {
        const std::uint16_t residentIndex = m_activeResidentIndices[activeIndex];
        if (residentHasFlag(m_residentEntries[residentIndex], ReadyMask))
        {
            ++count;
        }
    }
    return count;
}

std::uint16_t WorldGridFoliageCanopyManager::findResidentIndex(const WorldGridQuadtreeLeafId& leafId) const
{
    const std::uint16_t bucketIndex = bucketIndexForLeafId(leafId);
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

void WorldGridFoliageCanopyManager::insertResidentLookup(const WorldGridQuadtreeLeafId& leafId, std::uint16_t residentIndex)
{
    const std::uint16_t bucketIndex = bucketIndexForLeafId(leafId);
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

void WorldGridFoliageCanopyManager::removeResidentLookup(const WorldGridQuadtreeLeafId& leafId, std::uint16_t residentIndex)
{
    const std::uint16_t bucketIndex = bucketIndexForLeafId(leafId);
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

std::uint16_t WorldGridFoliageCanopyManager::bucketIndexForLeafId(const WorldGridQuadtreeLeafId& leafId)
{
    return static_cast<std::uint16_t>(hashLeafId(leafId) & (FoliageConfig::kCanopyLookupBucketCount - 1u));
}

bool WorldGridFoliageCanopyManager::enqueueLeaf(
    const WorldGridQuadtreeLeafId& leafId,
    const FoliageTerrainSource& terrainSource)
{
    for (std::uint16_t offset = 0; offset < m_queueCount; ++offset)
    {
        const std::uint16_t queueIndex = static_cast<std::uint16_t>((m_queueStart + offset) % kCapacity);
        if (m_leafQueue[queueIndex].leafId == leafId)
        {
            m_leafQueue[queueIndex].terrainSource = terrainSource;
            m_leafQueue[queueIndex].requestFrame = m_requestFrame;
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
        .requestFrame = m_requestFrame,
    };
    m_queueEnd = static_cast<std::uint16_t>((m_queueEnd + 1u) % kCapacity);
    ++m_queueCount;
    return true;
}

bool WorldGridFoliageCanopyManager::dequeueLeaf(QueuedLeafRequest& request)
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

std::uint16_t WorldGridFoliageCanopyManager::findOldestEvictableResidentIndex() const
{
    std::uint16_t oldestResidentIndex = kCapacity;
    std::uint8_t oldestAge = 0;

    for (std::uint16_t activeIndex = 0; activeIndex < m_residentCount; ++activeIndex)
    {
        const std::uint16_t residentIndex = m_activeResidentIndices[activeIndex];
        if (m_generationLocked[residentIndex])
        {
            continue;
        }

        const FoliageCanopyResidentCellEntry& entry = m_residentEntries[residentIndex];
        if (entry.evictionAge == 0)
        {
            continue;
        }

        if (oldestResidentIndex == kCapacity || entry.evictionAge > oldestAge)
        {
            oldestResidentIndex = residentIndex;
            oldestAge = entry.evictionAge;
        }
    }

    if (oldestResidentIndex != kCapacity)
    {
        return oldestResidentIndex;
    }

    for (std::uint16_t activeIndex = 0; activeIndex < m_residentCount; ++activeIndex)
    {
        const std::uint16_t residentIndex = m_activeResidentIndices[activeIndex];
        if (m_generationLocked[residentIndex])
        {
            continue;
        }

        const FoliageCanopyResidentCellEntry& entry = m_residentEntries[residentIndex];
        if (oldestResidentIndex == kCapacity || entry.evictionAge > oldestAge)
        {
            oldestResidentIndex = residentIndex;
            oldestAge = entry.evictionAge;
        }
    }

    return oldestResidentIndex;
}

void WorldGridFoliageCanopyManager::assignResidentCell(std::uint16_t residentIndex, const WorldGridQuadtreeLeafId& leafId)
{
    if (!m_residentUsed[residentIndex])
    {
        m_residentUsed[residentIndex] = true;
        m_residentActiveSlots[residentIndex] = m_residentCount;
        m_activeResidentIndices[m_residentCount] = residentIndex;
    }
    m_residentEntries[residentIndex] = {
        .leafId = leafId,
        .slotIndex = residentIndex,
        .evictionAge = 0,
        .residentFrameAge = 0,
        .flags = 0,
    };
    setResidentFlag(m_residentEntries[residentIndex], ReadyMask, false);
    m_terrainSources[residentIndex] = {};
}

void WorldGridFoliageCanopyManager::clearResidentCell(std::uint16_t residentIndex, bool removeFromActiveSet)
{
    if (removeFromActiveSet && m_residentUsed[residentIndex] && m_residentCount > 0)
    {
        const std::uint16_t removedActiveIndex = m_residentActiveSlots[residentIndex];
        const std::uint16_t lastActiveIndex = static_cast<std::uint16_t>(m_residentCount - 1u);
        const std::uint16_t lastResidentIndex = m_activeResidentIndices[lastActiveIndex];

        m_activeResidentIndices[removedActiveIndex] = lastResidentIndex;
        m_residentActiveSlots[lastResidentIndex] = removedActiveIndex;
        m_activeResidentIndices[lastActiveIndex] = kCapacity;
        m_residentActiveSlots[residentIndex] = kCapacity;
        --m_residentCount;
    }

    m_residentUsed[residentIndex] = !removeFromActiveSet && m_residentUsed[residentIndex];
    m_generationLocked[residentIndex] = false;
    m_residentEntries[residentIndex] = {};
    m_terrainSources[residentIndex] = {};
}

void WorldGridFoliageCanopyManager::resetCacheState()
{
    m_queueStart = 0;
    m_queueEnd = 0;
    m_queueCount = 0;
    m_leafQueue.fill({});
    m_residentEntries.fill({});
    m_residentUsed.fill(false);
    m_generationLocked.fill(false);
    m_terrainSources.fill({});
    m_activeResidentIndices.fill(kCapacity);
    m_residentActiveSlots.fill(kCapacity);
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
