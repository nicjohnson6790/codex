#include "WorldGridQuadtreeHeightmapManager.hpp"

#include "AppConfig.hpp"
#include "QuadtreeMeshRenderer.hpp"

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

void WorldGridQuadtreeHeightmapManager::uploadFromQueue(QuadtreeMeshRenderer& meshRenderer)
{
    WorldGridQuadtreeLeafId leafId{};
    if (!dequeueLeaf(leafId))
    {
        return;
    }

    std::uint16_t residentIndex = findResidentIndex(leafId);
    if (residentIndex != kCapacity)
    {
        m_residentMap[residentIndex].age = 0;
        return;
    }

    std::uint16_t slice = 0;
    if (m_freeSlotCount > 0)
    {
        slice = m_freeSlots[--m_freeSlotCount];
        residentIndex = m_residentCount++;
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

    float* buffer = meshRenderer.getCopyBuffer(slice);
    if (buffer == nullptr)
    {
        enqueueLeaf(leafId);
        return;
    }

    const auto [a, b] = worldGridQuadtreeLeafBounds(leafId);
    m_noiseGenerator.fillNoise(a, b, buffer);

    m_residentMap[residentIndex] = {
        .leafId = leafId,
        .lruSlice = slice,
        .age = 0,
    };
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
