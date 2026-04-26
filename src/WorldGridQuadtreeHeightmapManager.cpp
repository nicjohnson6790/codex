#include "WorldGridQuadtreeHeightmapManager.hpp"

#include "AppConfig.hpp"
#include "PerformanceCapture.hpp"
#include "QuadtreeMeshRenderer.hpp"

#include <algorithm>
#include <limits>

WorldGridQuadtreeHeightmapManager::WorldGridQuadtreeHeightmapManager()
{
    for (std::uint16_t slot = 0; slot < kCapacity; ++slot)
    {
        m_freeSlots[slot] = static_cast<std::uint16_t>(kCapacity - 1 - slot);
    }
    m_freeSlotCount = kCapacity;
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

    for (std::uint16_t slot = 0; slot < kCapacity; ++slot)
    {
        m_leafQueue[slot] = {};
        m_residentMap[slot] = {};
        m_knownExtents[slot] = {};
        m_knownExtentsValid[slot] = false;
        m_freeSlots[slot] = static_cast<std::uint16_t>(kCapacity - 1 - slot);
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

        m_knownExtentsValid[slice] = false;
        m_residentMap[residentIndex] = {
            .leafId = leafId,
            .lruSlice = slice,
            .age = 0,
        };
        if (appendResident)
        {
            --m_freeSlotCount;
            ++m_residentCount;
        }
    }
}

std::uint16_t WorldGridQuadtreeHeightmapManager::findResidentIndex(const WorldGridQuadtreeLeafId& leafId) const
{
    for (std::uint16_t index = 0; index < m_residentCount; ++index)
    {
        if (m_residentMap[index].leafId == leafId)
        {
            return index;
        }
    }

    return kCapacity;
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
