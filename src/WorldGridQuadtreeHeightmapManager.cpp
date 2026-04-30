#include "WorldGridQuadtreeHeightmapManager.hpp"

#include "AppConfig.hpp"
#include "PerformanceCapture.hpp"
#include "QuadtreeMeshRenderer.hpp"

#include <algorithm>
#include <bit>
#include <limits>

WorldGridQuadtreeHeightmapManager::WorldGridQuadtreeHeightmapManager()
{
    clearCache();
}

void WorldGridQuadtreeHeightmapManager::ageMap()
{
    for (std::uint16_t index = 0; index < m_residentCount; ++index)
    {
        ResidentMapEntry& entry = m_residentMap[index];
        if (entry.age < std::numeric_limits<std::uint8_t>::max())
        {
            ++entry.age;
        }
    }
}

void WorldGridQuadtreeHeightmapManager::clearCache()
{
    m_queueStart = 0;
    m_queueEnd = 0;
    m_queueCount = 0;
    m_residentCount = 0;
    m_lookupOverflowCount = 0;

    for (std::uint16_t slot = 0; slot < kCapacity; ++slot)
    {
        m_leafQueue[slot] = {};
        m_residentMap[slot] = {};
        m_knownExtents[slot] = {};
        m_knownExtentsValid[slot] = false;
        m_freeSlots[slot] = static_cast<std::uint16_t>(kCapacity - 1 - slot);
    }

    for (std::uint16_t bucketIndex = 0; bucketIndex < kLookupBucketCount; ++bucketIndex)
    {
        m_lookupBuckets[bucketIndex] = {};
        m_lookupBucketHasOverflow[bucketIndex] = false;
    }

    for (LookupOverflowEntry& overflowEntry : m_lookupOverflowEntries)
    {
        overflowEntry = {};
    }

    m_freeSlotCount = kCapacity;
}

bool WorldGridQuadtreeHeightmapManager::makeResident(const WorldGridQuadtreeLeafId& leafId)
{
    const std::uint16_t residentIndex = findResidentIndex(leafId);
    if (residentIndex != kCapacity)
    {
        m_residentMap[residentIndex].age = 0;
        return true;
    }

    enqueueLeaf(leafId);
    return false;
}

void WorldGridQuadtreeHeightmapManager::requestLeaf(const WorldGridQuadtreeLeafId& leafId, QuadtreeMeshRenderer& meshRenderer)
{
    const std::uint16_t residentIndex = findResidentIndex(leafId);
    if (residentIndex == kCapacity)
    {
        return;
    }

    ResidentMapEntry& entry = m_residentMap[residentIndex];
    entry.age = 0;
    meshRenderer.addLeaf(leafId, entry.lruSlice);
}

bool WorldGridQuadtreeHeightmapManager::getExtents(const WorldGridQuadtreeLeafId& leafId, HeightmapExtents& extents) const
{
    const std::uint16_t residentIndex = findResidentIndex(leafId);
    if (residentIndex == kCapacity)
    {
        return false;
    }

    const std::uint16_t sliceIndex = m_residentMap[residentIndex].lruSlice;
    if (!m_knownExtentsValid[sliceIndex])
    {
        return false;
    }

    extents = m_knownExtents[sliceIndex];
    return true;
}

bool WorldGridQuadtreeHeightmapManager::getResidentSliceIndex(
    const WorldGridQuadtreeLeafId& leafId,
    std::uint16_t& sliceIndex) const
{
    const std::uint16_t residentIndex = findResidentIndex(leafId);
    if (residentIndex == kCapacity)
    {
        return false;
    }

    sliceIndex = m_residentMap[residentIndex].lruSlice;
    return true;
}

void WorldGridQuadtreeHeightmapManager::applyGeneratedExtents(
    const WorldGridQuadtreeLeafId& leafId,
    std::uint16_t sliceIndex,
    const HeightmapExtents& extents)
{
    const std::uint16_t residentIndex = findResidentIndex(leafId);
    if (residentIndex == kCapacity || m_residentMap[residentIndex].lruSlice != sliceIndex)
    {
        return;
    }

    m_knownExtents[sliceIndex] = extents;
    m_knownExtentsValid[sliceIndex] = true;
}

void WorldGridQuadtreeHeightmapManager::setComputeDispatchBudget(std::uint16_t budget)
{
    m_computeDispatchBudget = std::max<std::uint16_t>(1, budget);
}

void WorldGridQuadtreeHeightmapManager::dispatchFromQueue(QuadtreeMeshRenderer& meshRenderer)
{
    HELLO_PROFILE_SCOPE("WorldGridQuadtreeHeightmapManager::DispatchFromQueue");

    for (std::uint16_t dispatchIndex = 0; dispatchIndex < m_computeDispatchBudget; ++dispatchIndex)
    {
        HELLO_PROFILE_SCOPE("WorldGridQuadtreeHeightmapManager::DispatchQueuedLeaf");

        WorldGridQuadtreeLeafId leafId{};
        if (!dequeueLeaf(leafId))
        {
            return;
        }

        std::uint16_t residentIndex = findResidentIndex(leafId);
        if (residentIndex != kCapacity)
        {
            m_residentMap[residentIndex].age = 0;
            continue;
        }

        std::uint16_t slice = 0;
        bool appendResident = false;
        if (m_freeSlotCount > 0)
        {
            slice = m_freeSlots[m_freeSlotCount - 1];
            residentIndex = m_residentCount;
            appendResident = true;
        }
        else
        {
            residentIndex = findOldestResidentIndex();
            if (residentIndex == kCapacity)
            {
                enqueueLeaf(leafId);
                return;
            }
            slice = m_residentMap[residentIndex].lruSlice;
        }

        if (!meshRenderer.queueHeightmapGeneration(leafId, slice, m_noiseGenerator.settings()))
        {
            enqueueLeaf(leafId);
            return;
        }

        if (!appendResident)
        {
            removeResidentLookup(m_residentMap[residentIndex].leafId, residentIndex);
        }

        m_knownExtentsValid[slice] = false;
        m_residentMap[residentIndex] = {
            .leafId = leafId,
            .lruSlice = slice,
            .age = 0,
        };
        insertResidentLookup(leafId, residentIndex);
        if (appendResident)
        {
            --m_freeSlotCount;
            ++m_residentCount;
        }
    }
}

std::uint16_t WorldGridQuadtreeHeightmapManager::findResidentIndex(const WorldGridQuadtreeLeafId& leafId) const
{
    const std::uint8_t bucketIndex = bucketIndexForLeafId(leafId);
    const auto& bucket = m_lookupBuckets[bucketIndex];

    for (const LookupBucketEntry& bucketEntry : bucket)
    {
        if (bucketEntry.residentIndex == kCapacity)
        {
            continue;
        }

        if (m_residentMap[bucketEntry.residentIndex].leafId == leafId)
        {
            return bucketEntry.residentIndex;
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

void WorldGridQuadtreeHeightmapManager::insertResidentLookup(const WorldGridQuadtreeLeafId& leafId, std::uint16_t residentIndex)
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

void WorldGridQuadtreeHeightmapManager::removeResidentLookup(const WorldGridQuadtreeLeafId& leafId, std::uint16_t residentIndex)
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

std::uint64_t WorldGridQuadtreeHeightmapManager::mix64(std::uint64_t x)
{
    x ^= x >> 30U;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27U;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31U;
    return x;
}

std::uint64_t WorldGridQuadtreeHeightmapManager::hashLeafId(const WorldGridQuadtreeLeafId& leafId)
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

std::uint8_t WorldGridQuadtreeHeightmapManager::bucketIndexForLeafId(const WorldGridQuadtreeLeafId& leafId)
{
    return static_cast<std::uint8_t>(hashLeafId(leafId) >> 56U);
}

bool WorldGridQuadtreeHeightmapManager::queueContains(const WorldGridQuadtreeLeafId& leafId) const
{
    for (std::uint16_t offset = 0; offset < m_queueCount; ++offset)
    {
        const std::uint16_t queueIndex = static_cast<std::uint16_t>((m_queueStart + offset) % kCapacity);
        if (m_leafQueue[queueIndex] == leafId)
        {
            return true;
        }
    }

    return false;
}

bool WorldGridQuadtreeHeightmapManager::enqueueLeaf(const WorldGridQuadtreeLeafId& leafId)
{
    if (queueContains(leafId) || m_queueCount >= kCapacity)
    {
        return false;
    }

    m_leafQueue[m_queueEnd] = leafId;
    m_queueEnd = static_cast<std::uint16_t>((m_queueEnd + 1) % kCapacity);
    ++m_queueCount;
    return true;
}

bool WorldGridQuadtreeHeightmapManager::dequeueLeaf(WorldGridQuadtreeLeafId& leafId)
{
    if (m_queueCount == 0)
    {
        return false;
    }

    leafId = m_leafQueue[m_queueStart];
    m_queueStart = static_cast<std::uint16_t>((m_queueStart + 1) % kCapacity);
    --m_queueCount;
    return true;
}

std::uint16_t WorldGridQuadtreeHeightmapManager::findOldestResidentIndex() const
{
    std::uint16_t oldestIndex = kCapacity;
    std::uint8_t oldestAge = 0;

    for (std::uint16_t index = 0; index < m_residentCount; ++index)
    {
        const ResidentMapEntry& entry = m_residentMap[index];
        if (entry.age == 0)
        {
            continue;
        }

        if (oldestIndex == kCapacity || entry.age > oldestAge)
        {
            oldestIndex = index;
            oldestAge = entry.age;
        }
    }

    return oldestIndex;
}
