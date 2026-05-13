#pragma once

#include "AppConfig.hpp"
#include "CameraManager.hpp"
#include "FoliageTypes.hpp"
#include "RenderTypes.hpp"
#include "WorldGridQuadtreeDebugRenderer.hpp"
#include "WorldGridQuadtreeHeightmapManager.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

class RenderEngines;
class FoliageCanopyRenderer;
class FoliageImposterRenderer;
class NearbyFoliageRenderer;
class QuadtreeMeshRenderer;
class WorldGridFoliageManager;
class WorldGridFoliageCanopyManager;
class WorldGridQuadtreeWaterManager;

struct QuadtreeNode
{
    WorldGridQuadtreeLeafId nodeId{};
    std::uint16_t parentIndex = std::uint16_t{ 0xFFFF };
    std::uint64_t currentStateStartFrame = 0;
    std::array<std::uint16_t, 4> children{
        std::uint16_t{ 0xFFFF },
        std::uint16_t{ 0xFFFF },
        std::uint16_t{ 0xFFFF },
        std::uint16_t{ 0xFFFF },
    };

    static constexpr std::uint16_t NullNodeIndex = UINT16_MAX;
};

class WorldGridQuadtree
{
public:
    struct TreeData
    {
        std::uint16_t drawableNodeCount = 0;
        std::uint16_t subdivisionCountThisFrame = 0;
        std::uint16_t collapseCountThisFrame = 0;
        std::uint32_t maxDepth = 0;
        std::array<std::uint16_t, 16> terrainDrawCountByScalePow{};
        std::array<std::uint16_t, 16> terrainLeafCountByScalePow{};
    };

    static constexpr std::size_t kNodeCapacity = 512;
    static constexpr std::size_t kBaseNodeCount = 9;

    WorldGridQuadtree();

    WorldGridQuadtree(const WorldGridQuadtree&) = delete;
    WorldGridQuadtree& operator=(const WorldGridQuadtree&) = delete;

    void updateTree(
        const CameraManager::Camera& activeCamera,
        Extent2D viewportExtent,
        QuadtreeMeshRenderer& meshRenderer,
        std::uint64_t frameIndex);
    void emitSceneDraws(
        RenderEngines& renderEngines,
        WorldGridFoliageManager* foliageManager,
        WorldGridFoliageCanopyManager* canopyManager,
        FoliageImposterRenderer* foliageRenderer,
        NearbyFoliageRenderer* nearbyFoliageRenderer,
        FoliageCanopyRenderer* canopyRenderer,
        WorldGridQuadtreeWaterManager* waterManager);
    void emitDebugDraws(RenderEngines& renderEngines) const;
    void clearTerrainCache();
    void setWaterVisibilityBounds(float waterMinHeight, float waterMaxHeight, bool enabled);
    [[nodiscard]] TerrainNoiseSettings& terrainSettings() { return m_heightmapManager.terrainSettings(); }
    [[nodiscard]] const TerrainNoiseSettings& terrainSettings() const { return m_heightmapManager.terrainSettings(); }
    [[nodiscard]] WorldGridQuadtreeHeightmapManager& heightmapManager() { return m_heightmapManager; }
    [[nodiscard]] const WorldGridQuadtreeHeightmapManager& heightmapManager() const { return m_heightmapManager; }
    [[nodiscard]] std::uint16_t computeDispatchBudget() const { return m_heightmapManager.computeDispatchBudget(); }
    void setComputeDispatchBudget(std::uint16_t budget) { m_heightmapManager.setComputeDispatchBudget(budget); }
    [[nodiscard]] std::uint16_t residentCount() const { return m_heightmapManager.residentCount(); }
    [[nodiscard]] std::uint16_t queuedCount() const { return m_heightmapManager.queuedCount(); }

    TreeData treeData{};

private:
    static constexpr std::uint32_t kQuadrantCount = 4;

    enum class LodDecision
    {
        Stay,
        Subdivide,
        Collapse,
    };

    void reset();
    void refreshBaseNodes(const Position& cameraPosition);
    [[nodiscard]] std::uint16_t allocateNode();
    void freeNode(std::uint16_t nodeIndex);
    void freeSubtree(std::uint16_t nodeIndex);
    void ensureChildren(std::uint16_t nodeIndex, const std::array<WorldGridQuadtreeLeafId, 4>& childIds);
    void updateNode(std::uint16_t nodeIndex, const CameraManager::Camera& activeCamera);
    [[nodiscard]] LodDecision evaluateLodPolicy(const QuadtreeNode& node, const CameraManager::Camera& activeCamera) const;
    [[nodiscard]] bool nodeOccupied(std::uint16_t nodeIndex) const;
    [[nodiscard]] static bool nodeHasChildren(const QuadtreeNode& node);
    [[nodiscard]] std::uint8_t quadrantInParent(std::uint16_t nodeIndex) const;
    [[nodiscard]] static std::array<WorldGridQuadtreeLeafId, 4> childIdsForNode(const QuadtreeNode& node);
    [[nodiscard]] bool collectNodeFoliagePageIds(
        const QuadtreeNode& node,
        std::array<WorldGridQuadtreeLeafId, 16>& canonicalLeafIds,
        std::uint32_t& canonicalLeafCount) const;
    [[nodiscard]] bool collectNodeCanopyCellIds(
        const QuadtreeNode& node,
        std::array<WorldGridQuadtreeLeafId, FoliageConfig::kCanopyCellCountPerNode>& canonicalLeafIds,
        std::uint32_t& canonicalLeafCount) const;
    [[nodiscard]] bool nodeIsVisible(const QuadtreeNode& node, HeightmapExtents* extents = nullptr) const;
    [[nodiscard]] bool nodeIntersectsCanopyRange(const QuadtreeNode& node) const;
    [[nodiscard]] bool nodeIsInNearbyFoliageTopology(const QuadtreeNode& node) const;
    [[nodiscard]] bool nodeIntersectsNearbyFoliageRange(const QuadtreeNode& node, const HeightmapExtents& extents) const;
    [[nodiscard]] bool nodeUsesCanonicalFoliagePages(const QuadtreeNode& node) const;
    [[nodiscard]] bool nodeCanUseCanopy(const QuadtreeNode& node) const;
    [[nodiscard]] bool nodeCanUseCanopyFallback(const QuadtreeNode& node) const;
    [[nodiscard]] bool nodeIsCanopyLod(const QuadtreeNode& node) const;
    [[nodiscard]] std::uint8_t nodeStateFadeAgeFrames(const QuadtreeNode& node) const;
    [[nodiscard]] std::uint8_t canopyEdgeFadeStrength(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool requestFoliageResidencyForNode(
        const QuadtreeNode& node,
        WorldGridFoliageManager& foliageManager) const;
    [[nodiscard]] bool requestCanopyResidencyForNode(
        const QuadtreeNode& node,
        WorldGridFoliageCanopyManager& canopyManager) const;
    [[nodiscard]] bool requestNearbyFoliageResidencyForNode(
        const QuadtreeNode& node,
        WorldGridFoliageManager& foliageManager,
        NearbyFoliageRenderer& nearbyFoliageRenderer) const;
    void emitTerrainDrawForNode(std::uint16_t nodeIndex, const QuadtreeNode& node, RenderEngines& renderEngines);
    [[nodiscard]] bool emitFoliageDrawForNode(
        const QuadtreeNode& node,
        WorldGridFoliageManager& foliageManager,
        FoliageImposterRenderer& foliageRenderer) const;
    void emitNearbyFoliageDrawForNode(
        const QuadtreeNode& node,
        WorldGridFoliageManager& foliageManager,
        NearbyFoliageRenderer& nearbyFoliageRenderer) const;
    [[nodiscard]] bool emitCanopyDrawForNode(
        std::uint16_t nodeIndex,
        const QuadtreeNode& node,
        WorldGridFoliageCanopyManager& canopyManager,
        FoliageCanopyRenderer& canopyRenderer) const;
    void emitWaterDrawForNode(std::uint16_t nodeIndex, const QuadtreeNode& node, WorldGridQuadtreeWaterManager& waterManager) const;
    [[nodiscard]] bool edgeHasDrawableNeighborCoverage(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool edgeHasDrawableCoarserNeighbor(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool edgeHasWaterNeighborCoverage(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool edgeHasWaterCoarserNeighbor(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] std::uint16_t findBaseNode(std::int64_t gridX, std::int64_t gridY) const;
    [[nodiscard]] std::uint16_t findNeighborSubtreeRoot(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool subtreeEdgeCoveredByTerrain(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool subtreeEdgeCoveredByWater(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    void recordTerrainLeafCount(const QuadtreeNode& node);
    [[nodiscard]] static double distanceSquaredToBounds(
        double pointX,
        double pointY,
        double pointZ,
        double minX,
        double minY,
        double minZ,
        double maxX,
        double maxY,
        double maxZ);

    WorldGridQuadtreeDebugRenderer m_debugRenderer;
    WorldGridQuadtreeHeightmapManager m_heightmapManager;
    std::array<QuadtreeNode, kNodeCapacity> m_nodes{};
    std::array<std::uint16_t, kNodeCapacity> m_freeNodes{};
    std::array<std::uint16_t, kBaseNodeCount> m_baseNodes{};
    std::uint16_t m_freeNodeCount = 0;
    std::int64_t m_baseGridX = 0;
    std::int64_t m_baseGridY = 0;
    bool m_hasBaseGrid = false;
    bool m_waterVisibilityBoundsEnabled = false;
    float m_waterVisibilityMinHeight = 0.0f;
    float m_waterVisibilityMaxHeight = 0.0f;
    std::int64_t m_nearbyCameraPageX = 0;
    std::int64_t m_nearbyCameraPageZ = 0;
    Position m_activeCameraPosition{};
    glm::dvec3 m_activeCameraForward{ 0.0, 0.0, -1.0 };
    glm::dvec3 m_activeCameraUp{ AppConfig::Camera::kWorldUp };
    Extent2D m_viewportExtent{ 16, 9 };
    std::uint64_t m_currentFrameIndex = 0;
};
