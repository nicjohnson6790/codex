#pragma once

#include "AppConfig.hpp"
#include "CameraManager.hpp"
#include "RenderTypes.hpp"
#include "WorldGridQuadtreeDebugRenderer.hpp"
#include "WorldGridQuadtreeHeightmapManager.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

class RenderEngines;
class QuadtreeMeshRenderer;
class WorldGridQuadtreeWaterManager;

struct QuadtreeNode
{
    WorldGridQuadtreeLeafId nodeId{};
    std::uint16_t parentIndex = std::uint16_t{0xFFFF};
    std::array<std::uint16_t, 4> children{
        std::uint16_t{0xFFFF},
        std::uint16_t{0xFFFF},
        std::uint16_t{0xFFFF},
        std::uint16_t{0xFFFF},
    };
    std::uint8_t quadrantInParent = 0;
    std::uint8_t flags = 0;
    float minHeight = 0.0f;
    float maxHeight = 0.0f;

    static constexpr std::uint16_t NullNodeIndex = UINT16_MAX;
    static constexpr std::uint8_t IsLeafMask = 1;
    static constexpr std::uint8_t ShouldDrawMask = 2;
    static constexpr std::uint8_t IsUsedMask = 4;
    // Parent remains the draw fallback while children finish becoming drawable.
    static constexpr std::uint8_t IsSubdividingMask = 8;
    // Existing children remain the draw fallback while the parent becomes drawable again.
    static constexpr std::uint8_t IsCollapsingMask = 16;
    // This node's own slice is not resident yet.
    static constexpr std::uint8_t IsUploadingMask = 32;
    static constexpr std::uint8_t HasExtentsMask = 64;
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
    };

    static constexpr std::size_t kNodeCapacity = 512;
    static constexpr std::size_t kBaseNodeCount = 9;

    WorldGridQuadtree();

    WorldGridQuadtree(const WorldGridQuadtree&) = delete;
    WorldGridQuadtree& operator=(const WorldGridQuadtree&) = delete;

    void beginHeightmapUpdate(QuadtreeMeshRenderer& meshRenderer);
    void updateTree(const CameraManager::Camera& activeCamera, Extent2D viewportExtent);
    void endHeightmapUpdate(QuadtreeMeshRenderer& meshRenderer);
    void emitMeshDraws(RenderEngines& renderEngines);
    void emitWaterDraws(WorldGridQuadtreeWaterManager& waterManager) const;
    void emitDebugDraws(RenderEngines& renderEngines) const;
    void clearTerrainCache();
    void setWaterVisibilityBounds(float waterMinHeight, float waterMaxHeight, bool enabled);
    [[nodiscard]] TerrainNoiseSettings& terrainSettings() { return m_heightmapManager.terrainSettings(); }
    [[nodiscard]] const TerrainNoiseSettings& terrainSettings() const { return m_heightmapManager.terrainSettings(); }
    [[nodiscard]] std::uint16_t computeDispatchBudget() const { return m_heightmapManager.computeDispatchBudget(); }
    void setComputeDispatchBudget(std::uint16_t budget) { m_heightmapManager.setComputeDispatchBudget(budget); }
    [[nodiscard]] std::uint16_t residentCount() const { return m_heightmapManager.residentCount(); }
    [[nodiscard]] std::uint16_t queuedCount() const { return m_heightmapManager.queuedCount(); }

    TreeData treeData{};

private:
    static constexpr std::uint32_t kQuadrantCount = 4;

    void reset();
    void refreshBaseNodes(const Position& cameraPosition);
    [[nodiscard]] std::uint16_t allocateNode();
    void freeNode(std::uint16_t nodeIndex);
    void freeSubtree(std::uint16_t nodeIndex);
    void ensureChildren(std::uint16_t nodeIndex);
    void updateNode(std::uint16_t nodeIndex, const CameraManager::Camera& activeCamera);
    void applyKnownExtentsToNode(QuadtreeNode& node);
    void applyGeneratedExtentsToKnownNodes(
        const WorldGridQuadtreeLeafId& leafId,
        const HeightmapExtents& extents);
    [[nodiscard]] static bool nodeContributesTerrainDraw(const QuadtreeNode& node);
    [[nodiscard]] static bool nodeHasResidentTerrainSurface(const QuadtreeNode& node);
    [[nodiscard]] static bool nodeContributesWaterDraw(const QuadtreeNode& node);
    [[nodiscard]] static bool nodeHasWaterSurface(const QuadtreeNode& node);
    [[nodiscard]] bool edgeHasDrawableNeighborCoverage(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool edgeHasDrawableCoarserNeighbor(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool edgeHasWaterNeighborCoverage(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool edgeHasWaterCoarserNeighbor(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] std::uint16_t findBaseNode(std::int64_t gridX, std::int64_t gridY) const;
    [[nodiscard]] std::uint16_t findNeighborSubtreeRoot(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool subtreeEdgeCoveredByTerrain(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool subtreeEdgeCoveredByWater(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool shouldSubdivide(const QuadtreeNode& node, const Position& cameraPosition) const;
    [[nodiscard]] static bool nodeHasFlag(const QuadtreeNode& node, std::uint8_t mask);
    static void setNodeFlag(QuadtreeNode& node, std::uint8_t mask, bool enabled);
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
    Extent2D m_viewportExtent{ 16, 9 };
};
