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
    std::uint8_t quadtreeLodHint)
{
    if (!m_settings.enabled || m_requestCount >= AppConfig::Water::kMaxWaterInstances)
    {
        return;
    }

    if (!shouldDrawWaterLeaf(leafOrigin, leafSizeMeters))
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
            request.bandMask);
    }
}

std::uint32_t WorldGridQuadtreeWaterManager::queuedCount() const
{
    return m_requestCount;
}

bool WorldGridQuadtreeWaterManager::shouldDrawWaterLeaf(const Position& leafOrigin, double leafSizeMeters) const
{
    (void)leafOrigin;
    return leafSizeMeters > 0.0;
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
    (void)leafSizeMeters;
    (void)distanceMeters;

    const std::uint32_t count = m_settings.cascadeCount;
    auto bit = [](std::uint32_t index) -> std::uint32_t
    {
        return 1u << index;
    };

    std::uint32_t mask = bit(0) | bit(1) | bit(2) | bit(3);
    if (count == 0)
    {
        return 0;
    }

    if (count < 32)
    {
        mask &= ((1u << count) - 1u);
    }

    return mask;
}
