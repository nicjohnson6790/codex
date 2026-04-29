#pragma once

#include "AppConfig.hpp"
#include "WaterSettings.hpp"
#include "WaterTypes.hpp"

#include <array>
#include <cstdint>

class QuadtreeWaterMeshRenderer;

class WorldGridQuadtreeWaterManager
{
public:
    WorldGridQuadtreeWaterManager();

    void beginFrame();
    void setActiveCamera(const Position& cameraPosition);
    void setWaterLevel(float waterLevel);
    [[nodiscard]] float waterLevel() const;
    [[nodiscard]] WaterSettings& settings();
    [[nodiscard]] const WaterSettings& settings() const;

    void requestLeaf(
        const WorldGridQuadtreeLeafId& leafId,
        const Position& leafOrigin,
        double leafSizeMeters,
        std::uint8_t quadtreeLodHint);

    void flushToRenderer(QuadtreeWaterMeshRenderer& renderer) const;
    [[nodiscard]] std::uint32_t queuedCount() const;

private:
    [[nodiscard]] bool shouldDrawWaterLeaf(const Position& leafOrigin, double leafSizeMeters) const;
    [[nodiscard]] double estimateDistanceToLeaf(const Position& leafOrigin, double leafSizeMeters) const;
    [[nodiscard]] std::uint32_t computeBandMask(double leafSizeMeters, double distanceMeters) const;

    WaterSettings m_settings{};
    Position m_activeCameraPosition{};
    std::array<WaterLeafDrawRequest, AppConfig::Water::kMaxWaterInstances> m_requests{};
    std::uint32_t m_requestCount = 0;
};
