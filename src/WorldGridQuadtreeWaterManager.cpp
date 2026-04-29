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
    m_requestsPerLod.fill(0);
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
    const std::uint8_t waterMeshLod = chooseWaterMeshLod(
        quadtreeLodHint,
        leafSizeMeters,
        distanceMeters);
    const std::uint32_t bandMask = computeBandMask(
        leafSizeMeters,
        distanceMeters,
        waterMeshLod);

    WaterLeafDrawRequest& request = m_requests[m_requestCount++];
    request.leafId = leafId;
    request.origin = leafOrigin;
    request.sizeMeters = leafSizeMeters;
    request.quadtreeLodHint = quadtreeLodHint;
    request.waterMeshLod = waterMeshLod;
    request.bandMask = bandMask;

    ++m_requestsPerLod[waterMeshLod];
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
            request.waterMeshLod,
            request.bandMask);
    }
}

std::uint32_t WorldGridQuadtreeWaterManager::queuedCount() const
{
    return m_requestCount;
}

std::uint32_t WorldGridQuadtreeWaterManager::queuedCountForLod(std::uint8_t lod) const
{
    if (lod >= AppConfig::Water::kMeshLodCount)
    {
        return 0;
    }

    return m_requestsPerLod[lod];
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

std::uint8_t WorldGridQuadtreeWaterManager::chooseWaterMeshLod(
    std::uint8_t quadtreeLodHint,
    double leafSizeMeters,
    double distanceMeters) const
{
    std::uint8_t lod = chooseWaterMeshLodFromHint(quadtreeLodHint);

    if (m_settings.lodPolicy.useDistanceOverride)
    {
        lod = std::max(lod, chooseWaterMeshLodFromDistance(distanceMeters));
    }

    if (m_settings.lodPolicy.useLeafSizeOverride)
    {
        lod = std::max(lod, chooseWaterMeshLodFromLeafSize(leafSizeMeters));
    }

    return clampToEnabledMeshLod(lod);
}

std::uint8_t WorldGridQuadtreeWaterManager::chooseWaterMeshLodFromHint(std::uint8_t quadtreeLodHint) const
{
    const std::uint8_t index = std::min<std::uint8_t>(
        quadtreeLodHint,
        static_cast<std::uint8_t>(m_settings.lodPolicy.meshLodForQuadtreeHint.size() - 1));
    const std::uint8_t lod = m_settings.lodPolicy.meshLodForQuadtreeHint[index];
    return std::min<std::uint8_t>(lod, static_cast<std::uint8_t>(AppConfig::Water::kMeshLodCount - 1));
}

std::uint8_t WorldGridQuadtreeWaterManager::chooseWaterMeshLodFromDistance(double distanceMeters) const
{
    for (std::uint8_t lod = 0; lod < AppConfig::Water::kMeshLodCount; ++lod)
    {
        if (distanceMeters <= m_settings.lodPolicy.maxDistanceForMeshLod[lod])
        {
            return lod;
        }
    }

    return static_cast<std::uint8_t>(AppConfig::Water::kMeshLodCount - 1);
}

std::uint8_t WorldGridQuadtreeWaterManager::chooseWaterMeshLodFromLeafSize(double leafSizeMeters) const
{
    for (std::uint8_t lod = 0; lod < AppConfig::Water::kMeshLodCount; ++lod)
    {
        if (leafSizeMeters <= m_settings.lodPolicy.maxLeafSizeForMeshLod[lod])
        {
            return lod;
        }
    }

    return static_cast<std::uint8_t>(AppConfig::Water::kMeshLodCount - 1);
}

std::uint8_t WorldGridQuadtreeWaterManager::clampToEnabledMeshLod(std::uint8_t lod) const
{
    const std::uint8_t maxLod = static_cast<std::uint8_t>(AppConfig::Water::kMeshLodCount - 1);
    lod = std::min(lod, maxLod);

    if (m_settings.meshLods[lod].enabled)
    {
        return lod;
    }

    for (std::uint8_t i = lod; i <= maxLod; ++i)
    {
        if (m_settings.meshLods[i].enabled)
        {
            return i;
        }
    }

    for (std::int32_t i = static_cast<std::int32_t>(lod) - 1; i >= 0; --i)
    {
        if (m_settings.meshLods[static_cast<std::uint8_t>(i)].enabled)
        {
            return static_cast<std::uint8_t>(i);
        }
    }

    return maxLod;
}

std::uint32_t WorldGridQuadtreeWaterManager::computeBandMask(
    double leafSizeMeters,
    double distanceMeters,
    std::uint8_t waterMeshLod) const
{
    (void)leafSizeMeters;
    (void)distanceMeters;

    const std::uint32_t count = m_settings.cascadeCount;
    auto bit = [](std::uint32_t index) -> std::uint32_t
    {
        return 1u << index;
    };

    std::uint32_t mask = 0;
    switch (waterMeshLod)
    {
    case 0:
    case 1:
        mask = bit(0) | bit(1) | bit(2) | bit(3);
        break;
    case 2:
        mask = bit(1) | bit(2) | bit(3) | bit(4);
        break;
    case 3:
        mask = bit(2) | bit(3) | bit(4) | bit(5);
        break;
    default:
        mask = bit(3) | bit(4) | bit(5);
        break;
    }

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
