#include "WorldGridQuadtreeWaterManager.hpp"

#include "QuadtreeWaterMeshRenderer.hpp"

#include <algorithm>

WorldGridQuadtreeWaterManager::WorldGridQuadtreeWaterManager()
    : m_settings(makeDefaultWaterSettings())
{
}

void WorldGridQuadtreeWaterManager::beginFrame()
{
    m_requestCount = 0;
}

void WorldGridQuadtreeWaterManager::setActiveCamera(const Position& cameraPosition)
{
    m_activeCameraPosition = cameraPosition;
}

void WorldGridQuadtreeWaterManager::setWaterLevel(float waterLevel)
{
    m_settings.waterLevel = waterLevel;
}

float WorldGridQuadtreeWaterManager::waterLevel() const
{
    return m_settings.waterLevel;
}

WaterSettings& WorldGridQuadtreeWaterManager::settings()
{
    return m_settings;
}

const WaterSettings& WorldGridQuadtreeWaterManager::settings() const
{
    return m_settings;
}

void WorldGridQuadtreeWaterManager::requestLeaf(
    const WorldGridQuadtreeLeafId& leafId,
    const Position& leafOrigin,
    double leafSizeMeters,
    bool terrainExtentsKnown,
    float terrainMinHeight,
    bool hasTerrainSlice,
    std::uint16_t terrainSliceIndex,
    std::uint8_t quadtreeLodHint)
{
    if (!m_settings.enabled || m_requestCount >= AppConfig::Water::kMaxWaterInstances)
    {
        return;
    }

    if (!shouldDrawWaterLeaf(leafOrigin, leafSizeMeters, terrainExtentsKnown, terrainMinHeight))
    {
        return;
    }

    const double distanceMeters = estimateDistanceToLeaf(leafOrigin, leafSizeMeters);
    const std::uint32_t bandMask = computeBandMask(leafSizeMeters, distanceMeters);

    WaterLeafDrawRequest& request = m_requests[m_requestCount++];
    request.leafId = leafId;
    request.origin = leafOrigin;
    request.sizeMeters = leafSizeMeters;
    request.quadtreeLodHint = quadtreeLodHint;
    request.bandMask = bandMask;
    request.terrainSliceIndex = terrainSliceIndex;
    request.hasTerrainSlice = hasTerrainSlice;
}

void WorldGridQuadtreeWaterManager::flushToRenderer(QuadtreeWaterMeshRenderer& renderer) const
{
    renderer.clear();
    renderer.setSettings(m_settings);
    for (std::uint32_t index = 0; index < m_requestCount; ++index)
    {
        const WaterLeafDrawRequest& request = m_requests[index];
        renderer.addLeaf(
            request.leafId,
            request.origin,
            request.sizeMeters,
            request.quadtreeLodHint,
            request.hasTerrainSlice,
            request.terrainSliceIndex,
            request.bandMask);
    }
}

std::uint32_t WorldGridQuadtreeWaterManager::queuedCount() const
{
    return m_requestCount;
}

bool WorldGridQuadtreeWaterManager::shouldDrawWaterLeaf(
    const Position& leafOrigin,
    double leafSizeMeters,
    bool terrainExtentsKnown,
    float terrainMinHeight) const
{
    (void)leafOrigin;
    if (leafSizeMeters <= 0.0)
    {
        return false;
    }

    if (!terrainExtentsKnown)
    {
        return true;
    }

    const float maxAllowedTerrainMinHeight =
        m_settings.waterLevel + m_settings.maxTerrainMinHeightAboveWaterToDraw;
    return terrainMinHeight <= maxAllowedTerrainMinHeight;
}

double WorldGridQuadtreeWaterManager::estimateDistanceToLeaf(const Position& leafOrigin, double leafSizeMeters) const
{
    const glm::dvec3 camera = m_activeCameraPosition.worldPosition();
    const glm::dvec3 origin = leafOrigin.worldPosition();
    const glm::dvec3 center{
        origin.x + (leafSizeMeters * 0.5),
        static_cast<double>(m_settings.waterLevel),
        origin.z + (leafSizeMeters * 0.5),
    };
    return glm::length(center - camera);
}

std::uint32_t WorldGridQuadtreeWaterManager::computeBandMask(
    double leafSizeMeters,
    double distanceMeters) const
{
    (void)distanceMeters;

    const std::uint32_t count = m_settings.cascadeCount;
    if (count == 0)
    {
        return 0;
    }

    const std::uint32_t activeCount =
        (leafSizeMeters <= 512.0) ? 4u :
        (leafSizeMeters <= 2048.0) ? 3u :
        (leafSizeMeters <= 8192.0) ? 2u :
        1u;

    const std::uint32_t clampedActiveCount = std::min(activeCount, count);
    const std::uint32_t firstCascadeIndex = count - clampedActiveCount;
    std::uint32_t mask = 0;
    for (std::uint32_t cascadeIndex = firstCascadeIndex; cascadeIndex < count; ++cascadeIndex)
    {
        mask |= (1u << cascadeIndex);
    }

    return mask;
}
