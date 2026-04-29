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
    [[nodiscard]] std::uint32_t queuedCountForLod(std::uint8_t lod) const;

private:
    [[nodiscard]] bool shouldDrawWaterLeaf(const Position& leafOrigin, double leafSizeMeters) const;
    [[nodiscard]] double estimateDistanceToLeaf(const Position& leafOrigin, double leafSizeMeters) const;
    [[nodiscard]] std::uint8_t chooseWaterMeshLod(
        std::uint8_t quadtreeLodHint,
        double leafSizeMeters,
        double distanceMeters) const;
    [[nodiscard]] std::uint8_t chooseWaterMeshLodFromHint(std::uint8_t quadtreeLodHint) const;
    [[nodiscard]] std::uint8_t chooseWaterMeshLodFromDistance(double distanceMeters) const;
    [[nodiscard]] std::uint8_t chooseWaterMeshLodFromLeafSize(double leafSizeMeters) const;
    [[nodiscard]] std::uint8_t clampToEnabledMeshLod(std::uint8_t lod) const;
    [[nodiscard]] std::uint32_t computeBandMask(
        double leafSizeMeters,
        double distanceMeters,
        std::uint8_t waterMeshLod) const;

    WaterSettings m_settings{};
    Position m_activeCameraPosition{};
    std::array<WaterLeafDrawRequest, AppConfig::Water::kMaxWaterInstances> m_requests{};
    std::array<std::uint32_t, AppConfig::Water::kMeshLodCount> m_requestsPerLod{};
    std::uint32_t m_requestCount = 0;
};
