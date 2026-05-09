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
    std::uint16_t parentIndex = std::uint16_t{0xFFFF};
    std::array<std::uint16_t, 4> children{
        std::uint16_t{0xFFFF},
        std::uint16_t{0xFFFF},
        std::uint16_t{0xFFFF},
        std::uint16_t{0xFFFF},
    };
    std::uint8_t quadrantInParent = 0;
    std::uint16_t flags = 0;
    float minHeight = 0.0f;
    float maxHeight = 0.0f;
    std::array<std::uint8_t, 4> canopyNeighborResidentAgeHints{
        std::uint8_t{255},
        std::uint8_t{255},
        std::uint8_t{255},
        std::uint8_t{255},
    };
    bool canRenderFoliageWithoutCanopyFallback = false;

    static constexpr std::uint16_t NullNodeIndex = UINT16_MAX;
    static constexpr std::uint16_t IsLeafMask = 1;
    static constexpr std::uint16_t ShouldDrawMask = 2;
    static constexpr std::uint16_t IsUsedMask = 4;
    // Parent remains the draw fallback while children finish becoming drawable.
    static constexpr std::uint16_t IsSubdividingMask = 8;
    // Existing children remain the draw fallback while the parent becomes drawable again.
    static constexpr std::uint16_t IsCollapsingMask = 16;
    // This node's own slice is not resident yet.
    static constexpr std::uint16_t IsUploadingMask = 32;
    static constexpr std::uint16_t HasExtentsMask = 64;
    static constexpr std::uint16_t FoliageUploadPendingMask = 128;
    static constexpr std::uint16_t FoliageShouldDrawMask = 256;
    static constexpr std::uint16_t SubdivisionHandoffMask = 512;
    static constexpr std::uint16_t CollapseHandoffMask = 1024;
    static constexpr std::uint16_t NearbyFoliageCpuResidentThisFrameMask = 2048;
    static constexpr std::uint16_t CanopyReadyMask = 4096;
    static constexpr std::uint16_t CanopyShouldDrawMask = 8192;
    static constexpr std::uint16_t CanRenderWithoutParentFallbackMask = 16384;
    static constexpr std::uint16_t MaintainCanopyResidencyMask = 32768;
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

    void beginHeightmapUpdate(QuadtreeMeshRenderer& meshRenderer);
    void updateTree(
        const CameraManager::Camera& activeCamera,
        Extent2D viewportExtent,
        WorldGridFoliageManager* foliageManager,
        WorldGridFoliageCanopyManager* canopyManager,
        NearbyFoliageRenderer* nearbyFoliageRenderer,
        std::uint64_t frameIndex);
    void endHeightmapUpdate(QuadtreeMeshRenderer& meshRenderer);
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
    void updateNode(std::uint16_t nodeIndex, const CameraManager::Camera& activeCamera, std::uint64_t frameIndex);
    void applyKnownExtentsToNode(QuadtreeNode& node);
    void applyGeneratedExtentsToKnownNodes(
        const WorldGridQuadtreeLeafId& leafId,
        const HeightmapExtents& extents);
    [[nodiscard]] bool collectNodeFoliagePageIds(
        const QuadtreeNode& node,
        std::array<WorldGridQuadtreeLeafId, 16>& canonicalLeafIds,
        std::uint32_t& canonicalLeafCount) const;
    [[nodiscard]] bool collectNodeCanopyCellIds(
        const QuadtreeNode& node,
        std::array<WorldGridQuadtreeLeafId, FoliageConfig::kCanopyCellCountPerNode>& canonicalLeafIds,
        std::uint32_t& canonicalLeafCount) const;
    [[nodiscard]] static bool nodeContributesTerrainDraw(const QuadtreeNode& node);
    [[nodiscard]] static bool nodeHasResidentTerrainSurface(const QuadtreeNode& node);
    [[nodiscard]] static bool nodeShouldDrawFoliage(const QuadtreeNode& node);
    [[nodiscard]] bool nodeIntersectsCanopyRange(const QuadtreeNode& node) const;
    [[nodiscard]] bool nodeShouldMaintainCanopyResidency(const QuadtreeNode& node) const;
    [[nodiscard]] bool nodeShouldDrawCanopy(const QuadtreeNode& node) const;
    [[nodiscard]] bool nodeShouldSuppressFoliageForCanopyTransition(
        std::uint16_t nodeIndex,
        const WorldGridFoliageCanopyManager& canopyManager) const;
    [[nodiscard]] static bool nodeContributesWaterDraw(const QuadtreeNode& node);
    [[nodiscard]] static bool nodeHasWaterSurface(const QuadtreeNode& node);
    [[nodiscard]] bool subtreeCanRenderFoliageWithoutCanopyFallback(std::uint16_t nodeIndex) const;
    void updateNodeFoliageState(QuadtreeNode& node, WorldGridFoliageManager* foliageManager);
    void updateNodeFoliageResidencyHints(const QuadtreeNode& node);
    void updateNodeCanopyResidencyHints(const QuadtreeNode& node);
    void updateNodeNearbyFoliageState(QuadtreeNode& node, std::uint64_t frameIndex);
    [[nodiscard]] bool nodeIsInNearbyFoliageTopology(const QuadtreeNode& node) const;
    [[nodiscard]] bool nodeIntersectsNearbyFoliageRange(const QuadtreeNode& node) const;
    void updateCanopyNeighborAgeHintsForNode(std::uint16_t nodeIndex);
    void emitTerrainDrawForNode(std::uint16_t nodeIndex, const QuadtreeNode& node, RenderEngines& renderEngines);
    void emitFoliageDrawForNode(
        std::uint16_t nodeIndex,
        const QuadtreeNode& node,
        WorldGridFoliageManager& foliageManager,
        WorldGridFoliageCanopyManager& canopyManager,
        FoliageImposterRenderer& foliageRenderer) const;
    void emitNearbyFoliageDrawForNode(const QuadtreeNode& node, NearbyFoliageRenderer& nearbyFoliageRenderer) const;
    void emitCanopyDrawForNode(
        std::uint16_t nodeIndex,
        const QuadtreeNode& node,
        WorldGridFoliageCanopyManager& canopyManager,
        FoliageCanopyRenderer& canopyRenderer) const;
    void emitWaterDrawForNode(std::uint16_t nodeIndex, const QuadtreeNode& node, WorldGridQuadtreeWaterManager& waterManager) const;
    [[nodiscard]] bool youngestCanopyResidentAgeForNode(std::uint16_t canopyNodeIndex, std::uint8_t& youngestAge) const;
    [[nodiscard]] bool edgeHasEqualLodNeighbor(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    void recordTerrainLeafCount(const QuadtreeNode& node);
    [[nodiscard]] bool edgeHasDrawableNeighborCoverage(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool edgeHasDrawableCoarserNeighbor(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool edgeHasWaterNeighborCoverage(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool edgeHasWaterCoarserNeighbor(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] std::uint16_t findBaseNode(std::int64_t gridX, std::int64_t gridY) const;
    [[nodiscard]] std::uint16_t findNeighborSubtreeRoot(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool subtreeEdgeCoveredByTerrain(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool subtreeEdgeCoveredByWater(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const;
    [[nodiscard]] bool shouldSubdivide(const QuadtreeNode& node, const Position& cameraPosition) const;
    [[nodiscard]] static bool nodeHasFlag(const QuadtreeNode& node, std::uint16_t mask);
    static void setNodeFlag(QuadtreeNode& node, std::uint16_t mask, bool enabled);
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
    WorldGridFoliageManager* m_activeFoliageManager = nullptr;
    WorldGridFoliageCanopyManager* m_activeCanopyManager = nullptr;
    NearbyFoliageRenderer* m_activeNearbyFoliageRenderer = nullptr;
    Position m_activeCameraPosition{};
    Extent2D m_viewportExtent{ 16, 9 };
};
