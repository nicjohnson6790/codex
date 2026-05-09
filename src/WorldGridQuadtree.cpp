#include "WorldGridQuadtree.hpp"

#include "FoliageCanopyRenderer.hpp"
#include "FoliageImposterRenderer.hpp"
#include "FoliageTypes.hpp"
#include "NearbyFoliageRenderer.hpp"
#include "PerformanceCapture.hpp"
#include "QuadtreeMeshRenderer.hpp"
#include "WorldGridFoliageCanopyManager.hpp"
#include "WorldGridFoliageManager.hpp"
#include "WorldGridQuadtreeWaterManager.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <glm/geometric.hpp>
#include <limits>
#include <utility>
#include <vector>

namespace
{
constexpr double kVisibilityBoundsHalfHeight = 10000.0;
constexpr double kEdgeCoverageEpsilon = 1.0e-4;
constexpr double kNearbyFoliageTreeHeightPaddingMeters = 45.0;
constexpr std::uint8_t kEdgeWest = 0;
constexpr std::uint8_t kEdgeSouth = 1;
constexpr std::uint8_t kEdgeEast = 2;
constexpr std::uint8_t kEdgeNorth = 3;

void initializeBaseNode(
    QuadtreeNode& node,
    std::int64_t gridX,
    std::int64_t gridY)
{
    node.nodeId = {
        .gridX = gridX,
        .gridY = gridY,
        .subdivisionPath = 0,
    };
    node.parentIndex = QuadtreeNode::NullNodeIndex;
    node.children.fill(QuadtreeNode::NullNodeIndex);
    node.quadrantInParent = 0;
    node.flags = QuadtreeNode::IsUsedMask | QuadtreeNode::IsLeafMask;
}

glm::dvec3 nodeCenterAtZeroHeight(const Position& minCorner, const Position& maxCorner)
{
    const glm::dvec3 minWorld = minCorner.worldPosition();
    const glm::dvec3 maxWorld = maxCorner.worldPosition();
    return {
        (minWorld.x + maxWorld.x) * 0.5,
        0.0,
        (minWorld.z + maxWorld.z) * 0.5,
    };
}

bool shouldDrawNodeFrustum(
    const Position& minCornerPosition,
    const Position& maxCornerPosition,
    const CameraManager::Camera& activeCamera,
    Extent2D viewportExtent)
{
    const glm::dvec3 forward = glm::normalize(activeCamera.forward);
    glm::dvec3 right = glm::cross(forward, activeCamera.up);
    if (glm::length(right) <= 0.00001)
    {
        right = { 1.0, 0.0, 0.0 };
    }
    right = glm::normalize(right);
    const glm::dvec3 up = glm::normalize(glm::cross(right, forward));

    const glm::dvec3 minCorner = minCornerPosition.localCoordinatesInCellOf(activeCamera.position);
    const glm::dvec3 maxCorner = maxCornerPosition.localCoordinatesInCellOf(activeCamera.position);
    const glm::dvec3 center = (minCorner + maxCorner) * 0.5;
    const glm::dvec3 extents = (maxCorner - minCorner) * 0.5;

    const double aspectRatio =
        static_cast<double>(std::max(viewportExtent.width, 1u)) /
        static_cast<double>(std::max(viewportExtent.height, 1u));
    const double tanHalfVerticalFov = std::tan(AppConfig::Camera::kVerticalFovRadians * 0.5);
    const double tanHalfHorizontalFov = tanHalfVerticalFov * aspectRatio;
    const auto boxIsOutsidePlane = [&center, &extents](const glm::dvec3& planeNormal, double planeOffset)
    {
        const double projectedRadius =
            std::abs(planeNormal.x) * extents.x +
            std::abs(planeNormal.y) * extents.y +
            std::abs(planeNormal.z) * extents.z;
        const double signedDistance = glm::dot(planeNormal, center) + planeOffset;
        return (signedDistance + projectedRadius) < 0.0;
    };

    if (boxIsOutsidePlane(forward, -AppConfig::Camera::kNearPlane))
    {
        return false;
    }
    if (boxIsOutsidePlane(right + (forward * tanHalfHorizontalFov), 0.0))
    {
        return false;
    }
    if (boxIsOutsidePlane((-right) + (forward * tanHalfHorizontalFov), 0.0))
    {
        return false;
    }
    if (boxIsOutsidePlane(up + (forward * tanHalfVerticalFov), 0.0))
    {
        return false;
    }
    if (boxIsOutsidePlane((-up) + (forward * tanHalfVerticalFov), 0.0))
    {
        return false;
    }

    return true;
}

std::uint64_t mixLeafIdWord64(std::uint64_t x)
{
    x ^= x >> 30U;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27U;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31U;
    return x;
}

std::uint32_t foliageLeafSeed(const WorldGridQuadtreeLeafId& leafId)
{
    const std::uint64_t word0 = std::bit_cast<std::uint64_t>(leafId.gridX);
    const std::uint64_t word1 = std::bit_cast<std::uint64_t>(leafId.gridY);
    const std::uint64_t word2 = leafId.subdivisionPath;

    std::uint64_t hash = 0x9e3779b97f4a7c15ULL;
    hash ^= mixLeafIdWord64(word0 + 0x9e3779b97f4a7c15ULL);
    hash = mixLeafIdWord64(hash);
    hash ^= mixLeafIdWord64(word1 + 0xbf58476d1ce4e5b9ULL);
    hash = mixLeafIdWord64(hash);
    hash ^= mixLeafIdWord64(word2 + 0x94d049bb133111ebULL);
    hash = mixLeafIdWord64(hash);
    return static_cast<std::uint32_t>(hash);
}

std::int64_t pageCoordinateForLeafAxis(std::int64_t grid, double localMin, std::uint32_t pageSizeMeters)
{
    const std::int64_t pagesPerCell =
        Position::kCellSize / static_cast<std::int64_t>(pageSizeMeters);
    const std::int64_t localPage =
        static_cast<std::int64_t>(std::llround(localMin / static_cast<double>(pageSizeMeters)));
    return (grid * pagesPerCell) + localPage;
}

bool quadtreeNodeHasFlag(const QuadtreeNode& node, std::uint16_t mask)
{
    return (node.flags & mask) != 0;
}

bool nodeIsParentOfLeaves(
    const std::array<QuadtreeNode, WorldGridQuadtree::kNodeCapacity>& nodes,
    const QuadtreeNode& node)
{
    if (quadtreeNodeHasFlag(node, QuadtreeNode::IsLeafMask))
    {
        return false;
    }

    for (const std::uint16_t childIndex : node.children)
    {
        if (childIndex == QuadtreeNode::NullNodeIndex)
        {
            return false;
        }

        const QuadtreeNode& child = nodes[childIndex];
        if (!quadtreeNodeHasFlag(child, QuadtreeNode::IsUsedMask) ||
            !quadtreeNodeHasFlag(child, QuadtreeNode::IsLeafMask))
        {
            return false;
        }
    }

    return true;
}

bool nodeUsesCanonicalFoliagePages(const QuadtreeNode& node)
{
    const double nodeSize = worldGridQuadtreeLeafSize(node.nodeId);
    return
        nodeSize >= static_cast<double>(FoliageConfig::kPageSizeMeters) &&
        nodeSize <= static_cast<double>(FoliageConfig::kPageSizeMeters * 4u);
}

bool quadrantTouchesEdge(std::uint8_t quadrant, std::uint8_t edgeIndex)
{
    switch (edgeIndex)
    {
    case kEdgeWest:
        return quadrant == 1u || quadrant == 3u;
    case kEdgeSouth:
        return quadrant == 2u || quadrant == 3u;
    case kEdgeEast:
        return quadrant == 0u || quadrant == 2u;
    case kEdgeNorth:
        return quadrant == 0u || quadrant == 1u;
    default:
        return false;
    }
}

std::uint8_t oppositeEdge(std::uint8_t edgeIndex)
{
    switch (edgeIndex)
    {
    case kEdgeWest:
        return kEdgeEast;
    case kEdgeSouth:
        return kEdgeNorth;
    case kEdgeEast:
        return kEdgeWest;
    case kEdgeNorth:
        return kEdgeSouth;
    default:
        return edgeIndex;
    }
}

std::uint16_t adjacentSiblingQuadrant(std::uint8_t quadrant, std::uint8_t edgeIndex)
{
    switch (edgeIndex)
    {
    case kEdgeWest:
        return quadrant == 0u ? 1u : 3u;
    case kEdgeSouth:
        return quadrant == 0u ? 2u : 3u;
    case kEdgeEast:
        return quadrant == 1u ? 0u : 2u;
    case kEdgeNorth:
        return quadrant == 2u ? 0u : 1u;
    default:
        return QuadtreeNode::NullNodeIndex;
    }
}

std::uint8_t descendantQuadrantForNeighbor(std::uint8_t sourceQuadrant, std::uint8_t edgeIndex)
{
    switch (edgeIndex)
    {
    case kEdgeWest:
        return (sourceQuadrant == 0u || sourceQuadrant == 1u) ? 0u : 2u;
    case kEdgeSouth:
        return (sourceQuadrant == 0u || sourceQuadrant == 2u) ? 0u : 1u;
    case kEdgeEast:
        return (sourceQuadrant == 0u || sourceQuadrant == 1u) ? 1u : 3u;
    case kEdgeNorth:
        return (sourceQuadrant == 0u || sourceQuadrant == 2u) ? 2u : 3u;
    default:
        return 0u;
    }
}

std::array<std::uint8_t, 2> edgeChildQuadrants(std::uint8_t edgeIndex)
{
    switch (edgeIndex)
    {
    case kEdgeWest:
        return { 1u, 3u };
    case kEdgeSouth:
        return { 2u, 3u };
    case kEdgeEast:
        return { 0u, 2u };
    case kEdgeNorth:
        return { 0u, 1u };
    default:
        return { 0u, 0u };
    }
}

template<typename SurfacePredicate>
bool subtreeEdgeCoveredBy(
    const std::array<QuadtreeNode, WorldGridQuadtree::kNodeCapacity>& nodes,
    std::uint16_t nodeIndex,
    std::uint8_t edgeIndex,
    SurfacePredicate&& nodeHasSurface)
{
    if (nodeIndex == QuadtreeNode::NullNodeIndex)
    {
        return false;
    }

    const QuadtreeNode& node = nodes[nodeIndex];
    if (nodeHasSurface(node))
    {
        return true;
    }

    if (quadtreeNodeHasFlag(node, QuadtreeNode::IsLeafMask))
    {
        return false;
    }

    const auto childQuadrants = edgeChildQuadrants(edgeIndex);
    for (const std::uint8_t childQuadrant : childQuadrants)
    {
        if (!subtreeEdgeCoveredBy(nodes, node.children[childQuadrant], edgeIndex, nodeHasSurface))
        {
            return false;
        }
    }

    return true;
}

template<std::size_t Capacity>
void collectCanonicalFoliageLeafIds(
    const WorldGridQuadtreeLeafId& nodeId,
    std::array<WorldGridQuadtreeLeafId, Capacity>& canonicalLeafIds,
    std::uint32_t& canonicalLeafCount)
{
    canonicalLeafCount = 0;
    const double nodeSize = worldGridQuadtreeLeafSize(nodeId);
    if (nodeSize < static_cast<double>(FoliageConfig::kPageSizeMeters))
    {
        return;
    }

    const std::uint32_t tilesPerSide =
        static_cast<std::uint32_t>(std::llround(nodeSize / static_cast<double>(FoliageConfig::kPageSizeMeters)));
    if (tilesPerSide == 0u)
    {
        return;
    }

    if ((tilesPerSide * tilesPerSide) > Capacity)
    {
        return;
    }

    for (std::uint32_t tileZ = 0; tileZ < tilesPerSide; ++tileZ)
    {
        for (std::uint32_t tileX = 0; tileX < tilesPerSide; ++tileX)
        {
            WorldGridQuadtreeLeafId canonicalLeafId = nodeId;
            std::uint32_t localTileX = tileX;
            std::uint32_t localTileZ = tileZ;
            std::uint32_t currentTilesPerSide = tilesPerSide;

            while (currentTilesPerSide > 1u)
            {
                const std::uint32_t halfTilesPerSide = currentTilesPerSide >> 1u;
                const bool east = localTileX >= halfTilesPerSide;
                const bool north = localTileZ >= halfTilesPerSide;
                const std::uint32_t quadrant =
                    east ? (north ? 0u : 2u) :
                    (north ? 1u : 3u);
                canonicalLeafId.subdivisionPath =
                    WorldGridQuadtreeLeafId::appendChild(canonicalLeafId.subdivisionPath, quadrant);
                if (east)
                {
                    localTileX -= halfTilesPerSide;
                }
                if (north)
                {
                    localTileZ -= halfTilesPerSide;
                }
                currentTilesPerSide = halfTilesPerSide;
            }

            canonicalLeafIds[canonicalLeafCount++] = canonicalLeafId;
        }
    }
}
}

WorldGridQuadtree::WorldGridQuadtree() { reset(); }

void WorldGridQuadtree::beginHeightmapUpdate(QuadtreeMeshRenderer& meshRenderer)
{
    HELLO_PROFILE_SCOPE("WorldGridQuadtree::BeginHeightmapUpdate");

    std::vector<QuadtreeMeshRenderer::GeneratedHeightmapExtents> completedExtents;
    meshRenderer.collectCompletedHeightmapExtents(completedExtents);
    for (const QuadtreeMeshRenderer::GeneratedHeightmapExtents& generatedExtents : completedExtents)
    {
        m_heightmapManager.applyGeneratedExtents(
            generatedExtents.leafId,
            generatedExtents.sliceIndex,
            generatedExtents.extents);
        applyGeneratedExtentsToKnownNodes(
            generatedExtents.leafId,
            generatedExtents.extents);
    }
}

void WorldGridQuadtree::endHeightmapUpdate(QuadtreeMeshRenderer& meshRenderer)
{
    HELLO_PROFILE_SCOPE("WorldGridQuadtree::EndHeightmapUpdate");
    m_heightmapManager.scheduleQueuedGenerations(meshRenderer);
}

void WorldGridQuadtree::updateTree(
    const CameraManager::Camera& activeCamera,
    Extent2D viewportExtent,
    WorldGridFoliageManager* foliageManager,
    WorldGridFoliageCanopyManager* canopyManager,
    NearbyFoliageRenderer* nearbyFoliageRenderer,
    std::uint64_t frameIndex)
{
    HELLO_PROFILE_SCOPE("WorldGridQuadtree::UpdateTree");

    m_activeFoliageManager = foliageManager;
    m_activeCanopyManager = canopyManager;
    m_activeNearbyFoliageRenderer = nearbyFoliageRenderer;
    m_activeCameraPosition = activeCamera.position;
    m_viewportExtent = viewportExtent;
    const glm::dvec3 cameraWorld = activeCamera.position.worldPosition();
    m_nearbyCameraPageX = static_cast<std::int64_t>(
        std::floor(cameraWorld.x / static_cast<double>(FoliageConfig::kPageSizeMeters)));
    m_nearbyCameraPageZ = static_cast<std::int64_t>(
        std::floor(cameraWorld.z / static_cast<double>(FoliageConfig::kPageSizeMeters)));
    {
        HELLO_PROFILE_SCOPE("WorldGridQuadtree::RefreshBaseNodes");
        refreshBaseNodes(activeCamera.position);
    }
    {
        HELLO_PROFILE_SCOPE("WorldGridQuadtree::AgeHeightmapResidency");
        m_heightmapManager.ageMap();
    }
    if (m_activeFoliageManager != nullptr)
    {
        HELLO_PROFILE_SCOPE("WorldGridQuadtree::AgeFoliageResidency");
        m_activeFoliageManager->ageMap();
    }
    if (m_activeCanopyManager != nullptr)
    {
        HELLO_PROFILE_SCOPE("WorldGridQuadtree::AgeCanopyResidency");
        m_activeCanopyManager->ageMap();
    }
    treeData = {};

    {
        HELLO_PROFILE_SCOPE("WorldGridQuadtree::UpdateBaseNodes");
        for (const std::uint16_t nodeIndex : m_baseNodes)
        {
            if (nodeIndex != QuadtreeNode::NullNodeIndex)
            {
                updateNode(nodeIndex, activeCamera, frameIndex);
            }
        }
    }

    if (m_activeNearbyFoliageRenderer == nullptr)
    {
        for (QuadtreeNode& node : m_nodes)
        {
            setNodeFlag(node, QuadtreeNode::NearbyFoliageCpuResidentThisFrameMask, false);
        }
    }
}

void WorldGridQuadtree::emitSceneDraws(
    RenderEngines& renderEngines,
    WorldGridFoliageManager* foliageManager,
    WorldGridFoliageCanopyManager* canopyManager,
    FoliageImposterRenderer* foliageRenderer,
    NearbyFoliageRenderer* nearbyFoliageRenderer,
    FoliageCanopyRenderer* canopyRenderer,
    WorldGridQuadtreeWaterManager* waterManager)
{
    HELLO_PROFILE_SCOPE("WorldGridQuadtree::EmitSceneDraws");

    for (std::uint16_t nodeIndex = 0; nodeIndex < static_cast<std::uint16_t>(m_nodes.size()); ++nodeIndex)
    {
        const QuadtreeNode& node = m_nodes[nodeIndex];
        if (!nodeHasFlag(node, QuadtreeNode::IsUsedMask))
        {
            continue;
        }

        const bool emitTerrain = renderEngines.quadtreeMeshRenderer != nullptr &&
            nodeContributesTerrainDraw(node);
        const bool emitCanopy = canopyManager != nullptr &&
            canopyRenderer != nullptr &&
            nodeHasFlag(node, QuadtreeNode::CanopyShouldDrawMask);
        const bool emitFoliage = foliageManager != nullptr &&
            canopyManager != nullptr &&
            foliageRenderer != nullptr &&
            nodeHasFlag(node, QuadtreeNode::FoliageShouldDrawMask);
        const bool emitNearbyFoliage = nearbyFoliageRenderer != nullptr &&
            nodeHasFlag(node, QuadtreeNode::NearbyFoliageCpuResidentThisFrameMask);
        const bool emitWater = waterManager != nullptr && nodeContributesWaterDraw(node);
        if (!(emitTerrain || emitCanopy || emitFoliage || emitNearbyFoliage || emitWater))
        {
            continue;
        }

        if (emitTerrain)
        {
            emitTerrainDrawForNode(nodeIndex, node, renderEngines);
        }
        if (emitCanopy)
        {
            emitCanopyDrawForNode(nodeIndex, node, *canopyManager, *canopyRenderer);
        }
        if (emitFoliage)
        {
            emitFoliageDrawForNode(nodeIndex, node, *foliageManager, *canopyManager, *foliageRenderer);
        }
        if (emitNearbyFoliage)
        {
            emitNearbyFoliageDrawForNode(node, *nearbyFoliageRenderer);
        }
        if (emitWater)
        {
            emitWaterDrawForNode(nodeIndex, node, *waterManager);
        }
    }
}

void WorldGridQuadtree::emitTerrainDrawForNode(
    std::uint16_t nodeIndex,
    const QuadtreeNode& node,
    RenderEngines& renderEngines)
{
    if (!nodeContributesTerrainDraw(node))
    {
        return;
    }

    std::uint16_t sliceIndex = 0;
    if (!m_heightmapManager.getResidentSliceIndex(node.nodeId, sliceIndex))
    {
        return;
    }

    const std::uint8_t scalePow = worldGridQuadtreeLeafScalePow(node.nodeId);
    if (scalePow < treeData.terrainDrawCountByScalePow.size())
    {
        ++treeData.terrainDrawCountByScalePow[scalePow];
    }

    m_heightmapManager.requestLeaf(node.nodeId, *renderEngines.quadtreeMeshRenderer);

    for (std::uint8_t edgeIndex = 0; edgeIndex < 4; ++edgeIndex)
    {
        if (edgeHasDrawableCoarserNeighbor(nodeIndex, edgeIndex))
        {
            renderEngines.quadtreeMeshRenderer->addCoarseBridge(node.nodeId, sliceIndex, edgeIndex);
        }
        else
        {
            renderEngines.quadtreeMeshRenderer->addBridge(node.nodeId, sliceIndex, edgeIndex);
        }
    }
}

void WorldGridQuadtree::clearTerrainCache()
{
    m_heightmapManager.clearCache();
    for (QuadtreeNode& node : m_nodes)
    {
        setNodeFlag(node, QuadtreeNode::HasExtentsMask, false);
        node.minHeight = 0.0f;
        node.maxHeight = 0.0f;
    }
}

bool WorldGridQuadtree::collectNodeFoliagePageIds(
    const QuadtreeNode& node,
    std::array<WorldGridQuadtreeLeafId, 16>& canonicalLeafIds,
    std::uint32_t& canonicalLeafCount) const
{
    collectCanonicalFoliageLeafIds(node.nodeId, canonicalLeafIds, canonicalLeafCount);
    return canonicalLeafCount > 0;
}

bool WorldGridQuadtree::collectNodeCanopyCellIds(
    const QuadtreeNode& node,
    std::array<WorldGridQuadtreeLeafId, FoliageConfig::kCanopyCellCountPerNode>& canonicalLeafIds,
    std::uint32_t& canonicalLeafCount) const
{
    collectCanonicalFoliageLeafIds(node.nodeId, canonicalLeafIds, canonicalLeafCount);
    return canonicalLeafCount > 0;
}

void WorldGridQuadtree::setWaterVisibilityBounds(float waterMinHeight, float waterMaxHeight, bool enabled)
{
    m_waterVisibilityBoundsEnabled = enabled;
    m_waterVisibilityMinHeight = waterMinHeight;
    m_waterVisibilityMaxHeight = waterMaxHeight;
}

void WorldGridQuadtree::emitDebugDraws(RenderEngines& renderEngines) const
{
    HELLO_PROFILE_SCOPE("WorldGridQuadtree::EmitDebugDraws");

    for (const QuadtreeNode& node : m_nodes)
    {
        if (nodeHasFlag(node, QuadtreeNode::IsUsedMask) &&
            nodeHasFlag(node, QuadtreeNode::ShouldDrawMask))
        {
            m_debugRenderer.appendNodeBorder(
                renderEngines,
                node.nodeId,
                treeData.maxDepth,
                nodeHasFlag(node, QuadtreeNode::HasExtentsMask),
                node.minHeight,
                node.maxHeight);
        }
    }
}

void WorldGridQuadtree::emitFoliageDrawForNode(
    std::uint16_t nodeIndex,
    const QuadtreeNode& node,
    WorldGridFoliageManager& foliageManager,
    WorldGridFoliageCanopyManager& canopyManager,
    FoliageImposterRenderer& foliageRenderer) const
{
    if (!nodeShouldDrawFoliage(node))
    {
        return;
    }

    if (nodeShouldSuppressFoliageForCanopyTransition(nodeIndex, canopyManager))
    {
        return;
    }

    std::uint16_t terrainSliceIndex = 0;
    if (!m_heightmapManager.getResidentSliceIndex(node.nodeId, terrainSliceIndex))
    {
        return;
    }

    std::array<WorldGridQuadtreeLeafId, 16> canonicalLeafIds{};
    std::uint32_t canonicalLeafCount = 0;
    if (!collectNodeFoliagePageIds(node, canonicalLeafIds, canonicalLeafCount))
    {
        return;
    }

    std::array<FoliageReadyPageInfo, 16> pageInfos{};
    for (std::uint32_t pageIndex = 0; pageIndex < canonicalLeafCount; ++pageIndex)
    {
        if (!foliageManager.getReadyPageInfo(canonicalLeafIds[pageIndex], pageInfos[pageIndex]))
        {
            return;
        }
    }

    const auto [terrainLeafOrigin, terrainLeafMaxCorner] = worldGridQuadtreeLeafBounds(node.nodeId);
    (void)terrainLeafMaxCorner;
    const std::uint8_t terrainScalePow = worldGridQuadtreeLeafScalePow(node.nodeId);

    for (std::uint32_t pageIndex = 0; pageIndex < canonicalLeafCount; ++pageIndex)
    {
        const FoliageReadyPageInfo& pageInfo = pageInfos[pageIndex];
        const auto [pageOrigin, pageMaxCorner] = worldGridQuadtreeLeafBounds(canonicalLeafIds[pageIndex]);
        (void)pageMaxCorner;
        foliageRenderer.addPageDraw({
            .pageIndex = pageInfo.pageIndex,
            .liveCount = pageInfo.liveCount,
            .seed = pageInfo.seed,
            .pageOrigin = pageOrigin,
            .terrainLeafOrigin = terrainLeafOrigin,
            .terrainSliceIndex = terrainSliceIndex,
            .terrainScalePow = terrainScalePow,
        });
    }
}

void WorldGridQuadtree::emitNearbyFoliageDrawForNode(
    const QuadtreeNode& node,
    NearbyFoliageRenderer& nearbyFoliageRenderer) const
{
    if (!nodeHasFlag(node, QuadtreeNode::NearbyFoliageCpuResidentThisFrameMask))
    {
        return;
    }

    std::uint16_t terrainSliceIndex = 0;
    if (!m_heightmapManager.getResidentSliceIndex(node.nodeId, terrainSliceIndex))
    {
        return;
    }

    nearbyFoliageRenderer.addNearbyInstancesForPage(
        node.nodeId,
        terrainSliceIndex,
        m_activeCameraPosition,
        FoliageConfig::kNearbyDefaultRadiusMeters);
}

void WorldGridQuadtree::emitCanopyDrawForNode(
    std::uint16_t nodeIndex,
    const QuadtreeNode& node,
    WorldGridFoliageCanopyManager& canopyManager,
    FoliageCanopyRenderer& canopyRenderer) const
{
    if (!nodeShouldDrawCanopy(node))
    {
        return;
    }

    std::uint16_t terrainSliceIndex = 0;
    if (!m_heightmapManager.getResidentSliceIndex(node.nodeId, terrainSliceIndex))
    {
        return;
    }

    std::array<WorldGridQuadtreeLeafId, FoliageConfig::kCanopyCellCountPerNode> canonicalLeafIds{};
    std::uint32_t canonicalLeafCount = 0;
    if (!collectNodeCanopyCellIds(node, canonicalLeafIds, canonicalLeafCount))
    {
        return;
    }

    FoliageCanopyDrawReference drawReference{};
    drawReference.patchOrigin = worldGridQuadtreeLeafBounds(node.nodeId).first;
    drawReference.terrainLeafOrigin = drawReference.patchOrigin;
    drawReference.patchSizeMeters = static_cast<float>(worldGridQuadtreeLeafSize(node.nodeId));
    drawReference.terrainLeafSizeMeters = drawReference.patchSizeMeters;
    drawReference.terrainSliceIndex = terrainSliceIndex;
    drawReference.patchSeed = foliageLeafSeed(node.nodeId);
    drawReference.drawAgeFrames = FoliageConfig::kCanopyFadeInFrameCount;
    drawReference.edgeFadeStrengths.fill(0u);
    drawReference.cellSlotIndices.fill(UINT16_MAX);
    drawReference.cellSeeds.fill(0u);

    const std::uint32_t fadeFrameCount = std::max<std::uint32_t>(FoliageConfig::kCanopyFadeInFrameCount, 1u);
    for (std::uint8_t edgeIndex = 0; edgeIndex < 4u; ++edgeIndex)
    {
        const std::uint8_t neighborYoungestAge = node.canopyNeighborResidentAgeHints[edgeIndex];
        if (neighborYoungestAge == 255u)
        {
            continue;
        }

        const std::uint32_t neighborFade = std::min<std::uint32_t>(neighborYoungestAge, fadeFrameCount);
        drawReference.edgeFadeStrengths[edgeIndex] = static_cast<std::uint8_t>(
            ((fadeFrameCount - neighborFade) * 255u) / fadeFrameCount);
    }

    std::uint32_t readyCellCount = 0;
    std::uint8_t youngestResidentFrameAge = std::numeric_limits<std::uint8_t>::max();
    for (std::uint32_t cellIndex = 0; cellIndex < canonicalLeafCount; ++cellIndex)
    {
        FoliageCanopyReadyCellInfo cellInfo{};
        const bool cellReady = canopyManager.getReadyCellInfo(canonicalLeafIds[cellIndex], cellInfo);
        if (cellReady)
        {
            drawReference.cellSlotIndices[cellIndex] = cellInfo.slotIndex;
            drawReference.cellSeeds[cellIndex] = cellInfo.seed;
            youngestResidentFrameAge = std::min(youngestResidentFrameAge, cellInfo.residentFrameAge);
            ++readyCellCount;
        }
    }

    const std::uint32_t minimumReadyCellCount = std::min<std::uint32_t>(
        canonicalLeafCount,
        FoliageConfig::kCanopyMinimumReadyCellCount);
    if (readyCellCount < minimumReadyCellCount)
    {
        return;
    }

    drawReference.drawAgeFrames = std::min(youngestResidentFrameAge, FoliageConfig::kCanopyFadeInFrameCount);
    for (std::uint32_t cellIndex = 0; cellIndex < canonicalLeafCount; ++cellIndex)
    {
        if (drawReference.cellSlotIndices[cellIndex] != UINT16_MAX)
        {
            canopyManager.noteRenderedCell(canonicalLeafIds[cellIndex]);
        }
    }

    canopyRenderer.addCanopyDraw(drawReference);
}

void WorldGridQuadtree::emitWaterDrawForNode(
    std::uint16_t nodeIndex,
    const QuadtreeNode& node,
    WorldGridQuadtreeWaterManager& waterManager) const
{
    if (!nodeContributesWaterDraw(node))
    {
        return;
    }

    const auto [minCorner, maxCorner] = worldGridQuadtreeLeafBounds(node.nodeId);
    (void)maxCorner;
    const double leafSizeMeters = worldGridQuadtreeLeafSize(node.nodeId);
    const std::uint8_t quadtreeLodHint = std::min<std::uint8_t>(worldGridQuadtreeLeafScalePow(node.nodeId), 4u);
    std::uint16_t terrainSliceIndex = 0;
    const bool hasTerrainSlice = m_heightmapManager.getResidentSliceIndex(node.nodeId, terrainSliceIndex);
    const bool queuedLeaf = waterManager.requestLeaf(
        node.nodeId,
        minCorner,
        leafSizeMeters,
        nodeHasFlag(node, QuadtreeNode::HasExtentsMask),
        node.minHeight,
        hasTerrainSlice,
        terrainSliceIndex,
        quadtreeLodHint);
    if (!queuedLeaf)
    {
        return;
    }

    const std::uint32_t bandMask = waterManager.computeBandMaskForLeaf(minCorner, leafSizeMeters);
    for (std::uint8_t edgeIndex = 0; edgeIndex < 4u; ++edgeIndex)
    {
        if (edgeHasWaterNeighborCoverage(nodeIndex, edgeIndex))
        {
            waterManager.requestBridge(
                node.nodeId,
                minCorner,
                leafSizeMeters,
                hasTerrainSlice,
                terrainSliceIndex,
                quadtreeLodHint,
                bandMask,
                edgeIndex);
            continue;
        }

        if (edgeHasWaterCoarserNeighbor(nodeIndex, edgeIndex))
        {
            waterManager.requestCoarseBridge(
                node.nodeId,
                minCorner,
                leafSizeMeters,
                hasTerrainSlice,
                terrainSliceIndex,
                quadtreeLodHint,
                bandMask,
                edgeIndex);
        }
    }
}

void WorldGridQuadtree::reset()
{
    for (std::uint16_t nodeIndex = 0; nodeIndex < static_cast<std::uint16_t>(kNodeCapacity); ++nodeIndex)
    {
        m_nodes[nodeIndex] = {};
        m_nodes[nodeIndex].children.fill(QuadtreeNode::NullNodeIndex);
        m_freeNodes[nodeIndex] = static_cast<std::uint16_t>(kNodeCapacity - 1 - nodeIndex);
    }

    m_baseNodes.fill(QuadtreeNode::NullNodeIndex);
    m_freeNodeCount = static_cast<std::uint16_t>(kNodeCapacity);
    m_hasBaseGrid = false;
    treeData = {};
}

void WorldGridQuadtree::refreshBaseNodes(const Position& cameraPosition)
{
    const std::int64_t targetBaseGridX = cameraPosition.gridX();
    const std::int64_t targetBaseGridY = cameraPosition.gridY();

    if (m_hasBaseGrid &&
        m_baseGridX == targetBaseGridX &&
        m_baseGridY == targetBaseGridY)
    {
        return;
    }

    if (!m_hasBaseGrid)
    {
        reset();
        m_baseGridX = targetBaseGridX;
        m_baseGridY = targetBaseGridY;
        m_hasBaseGrid = true;

        std::size_t baseNodeArrayIndex = 0;
        for (int gridOffsetY = -AppConfig::Quadtree::kNeighborRadius; gridOffsetY <= AppConfig::Quadtree::kNeighborRadius; ++gridOffsetY)
        {
            for (int gridOffsetX = -AppConfig::Quadtree::kNeighborRadius; gridOffsetX <= AppConfig::Quadtree::kNeighborRadius; ++gridOffsetX)
            {
                const std::uint16_t nodeIndex = allocateNode();
                if (nodeIndex == QuadtreeNode::NullNodeIndex)
                {
                    return;
                }

                QuadtreeNode& node = m_nodes[nodeIndex];
                initializeBaseNode(
                    node,
                    targetBaseGridX + static_cast<std::int64_t>(gridOffsetX),
                    targetBaseGridY + static_cast<std::int64_t>(gridOffsetY));
                m_baseNodes[baseNodeArrayIndex++] = nodeIndex;
            }
        }
        return;
    }

    const auto oldBaseNodes = m_baseNodes;
    std::array<bool, kBaseNodeCount> reusedOldBaseNodes{};
    std::array<std::uint16_t, kBaseNodeCount> newBaseNodes{};
    newBaseNodes.fill(QuadtreeNode::NullNodeIndex);

    std::size_t baseNodeArrayIndex = 0;
    for (int gridOffsetY = -AppConfig::Quadtree::kNeighborRadius; gridOffsetY <= AppConfig::Quadtree::kNeighborRadius; ++gridOffsetY)
    {
        for (int gridOffsetX = -AppConfig::Quadtree::kNeighborRadius; gridOffsetX <= AppConfig::Quadtree::kNeighborRadius; ++gridOffsetX)
        {
            const std::int64_t targetGridX = targetBaseGridX + static_cast<std::int64_t>(gridOffsetX);
            const std::int64_t targetGridY = targetBaseGridY + static_cast<std::int64_t>(gridOffsetY);

            for (std::size_t oldIndex = 0; oldIndex < oldBaseNodes.size(); ++oldIndex)
            {
                const std::uint16_t candidateNodeIndex = oldBaseNodes[oldIndex];
                if (candidateNodeIndex == QuadtreeNode::NullNodeIndex || reusedOldBaseNodes[oldIndex])
                {
                    continue;
                }

                const QuadtreeNode& candidateNode = m_nodes[candidateNodeIndex];
                if (candidateNode.nodeId.gridX == targetGridX &&
                    candidateNode.nodeId.gridY == targetGridY &&
                    candidateNode.nodeId.subdivisionPath == 0)
                {
                    newBaseNodes[baseNodeArrayIndex] = candidateNodeIndex;
                    reusedOldBaseNodes[oldIndex] = true;
                    break;
                }
            }

            ++baseNodeArrayIndex;
        }
    }

    for (std::size_t oldIndex = 0; oldIndex < oldBaseNodes.size(); ++oldIndex)
    {
        const std::uint16_t nodeIndex = oldBaseNodes[oldIndex];
        if (nodeIndex != QuadtreeNode::NullNodeIndex && !reusedOldBaseNodes[oldIndex])
        {
            freeSubtree(nodeIndex);
        }
    }

    baseNodeArrayIndex = 0;
    for (int gridOffsetY = -AppConfig::Quadtree::kNeighborRadius; gridOffsetY <= AppConfig::Quadtree::kNeighborRadius; ++gridOffsetY)
    {
        for (int gridOffsetX = -AppConfig::Quadtree::kNeighborRadius; gridOffsetX <= AppConfig::Quadtree::kNeighborRadius; ++gridOffsetX)
        {
            if (newBaseNodes[baseNodeArrayIndex] == QuadtreeNode::NullNodeIndex)
            {
                const std::uint16_t nodeIndex = allocateNode();
                if (nodeIndex == QuadtreeNode::NullNodeIndex)
                {
                    return;
                }

                QuadtreeNode& node = m_nodes[nodeIndex];
                initializeBaseNode(
                    node,
                    targetBaseGridX + static_cast<std::int64_t>(gridOffsetX),
                    targetBaseGridY + static_cast<std::int64_t>(gridOffsetY));
                newBaseNodes[baseNodeArrayIndex] = nodeIndex;
            }

            ++baseNodeArrayIndex;
        }
    }

    m_baseNodes = newBaseNodes;
    m_baseGridX = targetBaseGridX;
    m_baseGridY = targetBaseGridY;
    m_hasBaseGrid = true;
}

std::uint16_t WorldGridQuadtree::allocateNode()
{
    if (m_freeNodeCount == 0)
    {
        return QuadtreeNode::NullNodeIndex;
    }

    const std::uint16_t nodeIndex = m_freeNodes[--m_freeNodeCount];
    m_nodes[nodeIndex] = {};
    m_nodes[nodeIndex].parentIndex = QuadtreeNode::NullNodeIndex;
    m_nodes[nodeIndex].children.fill(QuadtreeNode::NullNodeIndex);
    return nodeIndex;
}

void WorldGridQuadtree::freeNode(std::uint16_t nodeIndex)
{
    if (nodeIndex == QuadtreeNode::NullNodeIndex)
    {
        return;
    }

    m_nodes[nodeIndex] = {};
    m_nodes[nodeIndex].parentIndex = QuadtreeNode::NullNodeIndex;
    m_nodes[nodeIndex].children.fill(QuadtreeNode::NullNodeIndex);
    m_freeNodes[m_freeNodeCount++] = nodeIndex;
}

void WorldGridQuadtree::freeSubtree(std::uint16_t nodeIndex)
{
    if (nodeIndex == QuadtreeNode::NullNodeIndex)
    {
        return;
    }

    QuadtreeNode& node = m_nodes[nodeIndex];
    for (std::uint16_t& childIndex : node.children)
    {
        if (childIndex != QuadtreeNode::NullNodeIndex)
        {
            freeSubtree(childIndex);
            childIndex = QuadtreeNode::NullNodeIndex;
        }
    }

    freeNode(nodeIndex);
}

void WorldGridQuadtree::ensureChildren(std::uint16_t nodeIndex)
{
    QuadtreeNode& node = m_nodes[nodeIndex];
    if (!nodeHasFlag(node, QuadtreeNode::IsLeafMask))
    {
        return;
    }

    std::array<std::uint16_t, kQuadrantCount> childIndices{};
    childIndices.fill(QuadtreeNode::NullNodeIndex);

    for (std::uint32_t quadrant = 0; quadrant < kQuadrantCount; ++quadrant)
    {
        const std::uint16_t childIndex = allocateNode();
        if (childIndex == QuadtreeNode::NullNodeIndex)
        {
            for (std::uint16_t allocatedChildIndex : childIndices)
            {
                freeNode(allocatedChildIndex);
            }
            return;
        }

        QuadtreeNode& child = m_nodes[childIndex];
        child.nodeId = {
            .gridX = node.nodeId.gridX,
            .gridY = node.nodeId.gridY,
            .subdivisionPath = WorldGridQuadtreeLeafId::appendChild(node.nodeId.subdivisionPath, quadrant),
        };
        child.parentIndex = nodeIndex;
        child.children.fill(QuadtreeNode::NullNodeIndex);
        child.quadrantInParent = static_cast<std::uint8_t>(quadrant);
        child.flags = QuadtreeNode::IsUsedMask | QuadtreeNode::IsLeafMask;
        childIndices[quadrant] = childIndex;
    }

    node.children = childIndices;
    setNodeFlag(node, QuadtreeNode::IsLeafMask, false);
}

void WorldGridQuadtree::applyKnownExtentsToNode(QuadtreeNode& node)
{
    HeightmapExtents extents{};
    if (!m_heightmapManager.getExtents(node.nodeId, extents))
    {
        return;
    }

    node.minHeight = extents.minHeight;
    node.maxHeight = extents.maxHeight;
    setNodeFlag(node, QuadtreeNode::HasExtentsMask, true);
}

void WorldGridQuadtree::applyGeneratedExtentsToKnownNodes(
    const WorldGridQuadtreeLeafId& leafId,
    const HeightmapExtents& extents)
{
    for (QuadtreeNode& node : m_nodes)
    {
        if (!nodeHasFlag(node, QuadtreeNode::IsUsedMask) || node.nodeId != leafId)
        {
            continue;
        }

        node.minHeight = extents.minHeight;
        node.maxHeight = extents.maxHeight;
        setNodeFlag(node, QuadtreeNode::HasExtentsMask, true);
    }
}

bool WorldGridQuadtree::nodeContributesTerrainDraw(const QuadtreeNode& node)
{
    if (!nodeHasFlag(node, QuadtreeNode::IsUsedMask) ||
        !nodeHasFlag(node, QuadtreeNode::ShouldDrawMask))
    {
        return false;
    }

    if (nodeHasFlag(node, QuadtreeNode::IsLeafMask))
    {
        return !nodeHasFlag(node, QuadtreeNode::IsUploadingMask);
    }

    return
        nodeHasFlag(node, QuadtreeNode::IsSubdividingMask) &&
        !nodeHasFlag(node, QuadtreeNode::IsUploadingMask);
}

bool WorldGridQuadtree::nodeHasResidentTerrainSurface(const QuadtreeNode& node)
{
    if (!nodeHasFlag(node, QuadtreeNode::IsUsedMask))
    {
        return false;
    }

    if (nodeHasFlag(node, QuadtreeNode::IsLeafMask))
    {
        return !nodeHasFlag(node, QuadtreeNode::IsUploadingMask);
    }

    return
        nodeHasFlag(node, QuadtreeNode::IsSubdividingMask) &&
        !nodeHasFlag(node, QuadtreeNode::IsUploadingMask);
}

bool WorldGridQuadtree::nodeContributesWaterDraw(const QuadtreeNode& node)
{
    return nodeHasFlag(node, QuadtreeNode::ShouldDrawMask) && nodeHasWaterSurface(node);
}

bool WorldGridQuadtree::nodeHasWaterSurface(const QuadtreeNode& node)
{
    if (!nodeHasFlag(node, QuadtreeNode::IsUsedMask))
    {
        return false;
    }

    if (nodeHasFlag(node, QuadtreeNode::IsLeafMask))
    {
        return true;
    }

    return
        nodeHasFlag(node, QuadtreeNode::IsSubdividingMask) &&
        !nodeHasFlag(node, QuadtreeNode::IsUploadingMask);
}

bool WorldGridQuadtree::nodeShouldDrawFoliage(const QuadtreeNode& node)
{
    return
        nodeHasFlag(node, QuadtreeNode::FoliageShouldDrawMask) &&
        nodeUsesCanonicalFoliagePages(node);
}

bool WorldGridQuadtree::nodeIntersectsCanopyRange(const QuadtreeNode& node) const
{
    if (!nodeHasFlag(node, QuadtreeNode::IsUsedMask))
    {
        return false;
    }

    const double nodeSize = worldGridQuadtreeLeafSize(node.nodeId);
    const auto [nodeMinCorner, nodeMaxCorner] = worldGridQuadtreeLeafBounds(node.nodeId);
    const glm::dvec3 nodeMinWorld = nodeMinCorner.worldPosition();
    const glm::dvec3 nodeMaxWorld = nodeMaxCorner.worldPosition();
    const glm::dvec3 cameraWorld = m_activeCameraPosition.worldPosition();
    const double rangePadding = nodeSize;
    const double canopyRangeMeters = static_cast<double>(FoliageConfig::kCanopyFadeEndMeters) + rangePadding;
    return distanceSquaredToBounds(
        cameraWorld.x,
        cameraWorld.y,
        cameraWorld.z,
        nodeMinWorld.x,
        cameraWorld.y,
        nodeMinWorld.z,
        nodeMaxWorld.x,
        cameraWorld.y,
        nodeMaxWorld.z) <= (canopyRangeMeters * canopyRangeMeters);
}

bool WorldGridQuadtree::nodeShouldMaintainCanopyResidency(const QuadtreeNode& node) const
{
    return nodeHasFlag(node, QuadtreeNode::MaintainCanopyResidencyMask);
}

bool WorldGridQuadtree::nodeShouldDrawCanopy(const QuadtreeNode& node) const
{
    return nodeHasFlag(node, QuadtreeNode::CanopyShouldDrawMask);
}

bool WorldGridQuadtree::nodeShouldSuppressFoliageForCanopyTransition(
    std::uint16_t nodeIndex,
    const WorldGridFoliageCanopyManager& canopyManager) const
{
    (void)canopyManager;

    if (nodeShouldDrawCanopy(m_nodes[nodeIndex]))
    {
        return true;
    }

    std::uint16_t ancestorIndex = m_nodes[nodeIndex].parentIndex;
    while (ancestorIndex != QuadtreeNode::NullNodeIndex)
    {
        const QuadtreeNode& ancestor = m_nodes[ancestorIndex];
        if (nodeShouldMaintainCanopyResidency(ancestor))
        {
            return nodeShouldDrawCanopy(ancestor);
        }

        ancestorIndex = ancestor.parentIndex;
    }

    return false;
}

bool WorldGridQuadtree::subtreeCanRenderFoliageWithoutCanopyFallback(std::uint16_t nodeIndex) const
{
    if (nodeIndex == QuadtreeNode::NullNodeIndex)
    {
        return true;
    }

    const QuadtreeNode& node = m_nodes[nodeIndex];
    if (!nodeHasFlag(node, QuadtreeNode::IsUsedMask) ||
        !nodeHasFlag(node, QuadtreeNode::ShouldDrawMask))
    {
        return true;
    }
    return node.canRenderFoliageWithoutCanopyFallback;
}

void WorldGridQuadtree::updateNodeFoliageState(QuadtreeNode& node, WorldGridFoliageManager* foliageManager)
{
    if (foliageManager == nullptr ||
        !nodeHasFlag(node, QuadtreeNode::ShouldDrawMask) ||
        !nodeHasResidentTerrainSurface(node) ||
        !nodeUsesCanonicalFoliagePages(node))
    {
        setNodeFlag(node, QuadtreeNode::FoliageUploadPendingMask, false);
        setNodeFlag(node, QuadtreeNode::FoliageShouldDrawMask, false);
        return;
    }

    std::array<WorldGridQuadtreeLeafId, 16> canonicalLeafIds{};
    std::uint32_t canonicalLeafCount = 0;
    if (!collectNodeFoliagePageIds(node, canonicalLeafIds, canonicalLeafCount))
    {
        setNodeFlag(node, QuadtreeNode::FoliageUploadPendingMask, false);
        setNodeFlag(node, QuadtreeNode::FoliageShouldDrawMask, false);
        return;
    }

    bool allPagesReady = true;
    std::uint16_t terrainSliceIndex = 0;
    if (!m_heightmapManager.getResidentSliceIndex(node.nodeId, terrainSliceIndex))
    {
        setNodeFlag(node, QuadtreeNode::FoliageUploadPendingMask, false);
        setNodeFlag(node, QuadtreeNode::FoliageShouldDrawMask, false);
        return;
    }
    for (std::uint32_t pageIndex = 0; pageIndex < canonicalLeafCount; ++pageIndex)
    {
        allPagesReady = foliageManager->makeResident(
            canonicalLeafIds[pageIndex],
            node.nodeId,
            terrainSliceIndex) && allPagesReady;
    }

    setNodeFlag(node, QuadtreeNode::FoliageUploadPendingMask, !allPagesReady);
    setNodeFlag(node, QuadtreeNode::FoliageShouldDrawMask, allPagesReady);
}

void WorldGridQuadtree::updateNodeFoliageResidencyHints(const QuadtreeNode& node)
{
    if (m_activeFoliageManager == nullptr ||
        !nodeHasFlag(node, QuadtreeNode::IsLeafMask) ||
        nodeHasFlag(node, QuadtreeNode::ShouldDrawMask) ||
        !nodeHasResidentTerrainSurface(node) ||
        !nodeUsesCanonicalFoliagePages(node))
    {
        return;
    }

    std::uint16_t terrainSliceIndex = 0;
    if (!m_heightmapManager.getResidentSliceIndex(node.nodeId, terrainSliceIndex))
    {
        return;
    }

    std::array<WorldGridQuadtreeLeafId, 16> canonicalLeafIds{};
    std::uint32_t canonicalLeafCount = 0;
    if (!collectNodeFoliagePageIds(node, canonicalLeafIds, canonicalLeafCount))
    {
        return;
    }

    for (std::uint32_t pageIndex = 0; pageIndex < canonicalLeafCount; ++pageIndex)
    {
        (void)m_activeFoliageManager->makeResident(
            canonicalLeafIds[pageIndex],
            node.nodeId,
            terrainSliceIndex);
    }
}

void WorldGridQuadtree::updateNodeCanopyResidencyHints(const QuadtreeNode& node)
{
    if (m_activeCanopyManager == nullptr ||
        !nodeHasFlag(node, QuadtreeNode::IsLeafMask) ||
        nodeHasFlag(node, QuadtreeNode::ShouldDrawMask) ||
        !nodeHasResidentTerrainSurface(node) ||
        std::abs(worldGridQuadtreeLeafSize(node.nodeId) - static_cast<double>(FoliageConfig::kCanopyNodeSizeMeters)) > 0.001)
    {
        return;
    }

    std::uint16_t terrainSliceIndex = 0;
    if (!m_heightmapManager.getResidentSliceIndex(node.nodeId, terrainSliceIndex))
    {
        return;
    }

    std::array<WorldGridQuadtreeLeafId, FoliageConfig::kCanopyCellCountPerNode> canonicalLeafIds{};
    std::uint32_t canonicalLeafCount = 0;
    if (!collectNodeCanopyCellIds(node, canonicalLeafIds, canonicalLeafCount))
    {
        return;
    }

    for (std::uint32_t cellIndex = 0; cellIndex < canonicalLeafCount; ++cellIndex)
    {
        (void)m_activeCanopyManager->makeResident(canonicalLeafIds[cellIndex], node.nodeId, terrainSliceIndex);
    }
}

void WorldGridQuadtree::updateNodeNearbyFoliageState(QuadtreeNode& node, std::uint64_t frameIndex)
{
    if (m_activeFoliageManager == nullptr || m_activeNearbyFoliageRenderer == nullptr)
    {
        setNodeFlag(node, QuadtreeNode::NearbyFoliageCpuResidentThisFrameMask, false);
        return;
    }

    if (!nodeIsInNearbyFoliageTopology(node) ||
        !nodeIntersectsNearbyFoliageRange(node))
    {
        setNodeFlag(node, QuadtreeNode::NearbyFoliageCpuResidentThisFrameMask, false);
        return;
    }

    FoliageReadyPageInfo pageInfo{};
    if (!m_activeFoliageManager->getReadyPageInfo(node.nodeId, pageInfo))
    {
        setNodeFlag(node, QuadtreeNode::NearbyFoliageCpuResidentThisFrameMask, false);
        return;
    }

    const bool nearbyResident = m_activeNearbyFoliageRenderer->makeResident(node.nodeId, pageInfo, frameIndex);
    setNodeFlag(node, QuadtreeNode::NearbyFoliageCpuResidentThisFrameMask, nearbyResident);
}

bool WorldGridQuadtree::nodeIsInNearbyFoliageTopology(const QuadtreeNode& node) const
{
    if (!nodeHasFlag(node, QuadtreeNode::IsUsedMask) ||
        !nodeHasFlag(node, QuadtreeNode::IsLeafMask) ||
        !nodeHasResidentTerrainSurface(node) ||
        std::abs(worldGridQuadtreeLeafSize(node.nodeId) - static_cast<double>(FoliageConfig::kPageSizeMeters)) > 0.001)
    {
        return false;
    }

    double localMinX = 0.0;
    double localMinZ = 0.0;
    double size = 0.0;
    worldGridQuadtreeLeafExtents(node.nodeId, localMinX, localMinZ, size);
    (void)size;
    const std::int64_t nodePageX =
        pageCoordinateForLeafAxis(node.nodeId.gridX, localMinX, FoliageConfig::kPageSizeMeters);
    const std::int64_t nodePageZ =
        pageCoordinateForLeafAxis(node.nodeId.gridY, localMinZ, FoliageConfig::kPageSizeMeters);
    return
        std::abs(nodePageX - m_nearbyCameraPageX) <= 1 &&
        std::abs(nodePageZ - m_nearbyCameraPageZ) <= 1;
}

bool WorldGridQuadtree::nodeIntersectsNearbyFoliageRange(const QuadtreeNode& node) const
{
    if (!nodeHasFlag(node, QuadtreeNode::HasExtentsMask))
    {
        return false;
    }

    const double minHeight = static_cast<double>(node.minHeight);
    const double maxHeight = static_cast<double>(node.maxHeight) + kNearbyFoliageTreeHeightPaddingMeters;
    const auto [minCorner, maxCorner] = worldGridQuadtreeLeafBounds(
        node.nodeId,
        minHeight,
        maxHeight);
    const glm::dvec3 minWorld = minCorner.worldPosition();
    const glm::dvec3 maxWorld = maxCorner.worldPosition();
    const glm::dvec3 cameraWorld = m_activeCameraPosition.worldPosition();
    const double range =
        static_cast<double>(FoliageConfig::kNearbyDefaultRadiusMeters) +
        static_cast<double>(FoliageConfig::kNearbyDecodeRangePaddingMeters);

    return distanceSquaredToBounds(
        cameraWorld.x,
        cameraWorld.y,
        cameraWorld.z,
        minWorld.x,
        minWorld.y,
        minWorld.z,
        maxWorld.x,
        maxWorld.y,
        maxWorld.z) <= (range * range);
}

void WorldGridQuadtree::recordTerrainLeafCount(const QuadtreeNode& node)
{
    if (!nodeHasFlag(node, QuadtreeNode::IsUsedMask) ||
        !nodeHasFlag(node, QuadtreeNode::IsLeafMask))
    {
        return;
    }

    const std::uint8_t scalePow = worldGridQuadtreeLeafScalePow(node.nodeId);
    if (scalePow < treeData.terrainLeafCountByScalePow.size())
    {
        ++treeData.terrainLeafCountByScalePow[scalePow];
    }
}

bool WorldGridQuadtree::youngestCanopyResidentAgeForNode(std::uint16_t canopyNodeIndex, std::uint8_t& youngestAge) const
{
    if (canopyNodeIndex == QuadtreeNode::NullNodeIndex || m_activeCanopyManager == nullptr)
    {
        return false;
    }

    const QuadtreeNode& canopyNode = m_nodes[canopyNodeIndex];
    if (!nodeHasFlag(canopyNode, QuadtreeNode::IsUsedMask) ||
        std::abs(worldGridQuadtreeLeafSize(canopyNode.nodeId) - static_cast<double>(FoliageConfig::kCanopyNodeSizeMeters)) > 0.001)
    {
        return false;
    }

    std::array<WorldGridQuadtreeLeafId, FoliageConfig::kCanopyCellCountPerNode> canopyLeafIds{};
    std::uint32_t canopyLeafCount = 0;
    if (!collectNodeCanopyCellIds(canopyNode, canopyLeafIds, canopyLeafCount))
    {
        return false;
    }

    std::uint32_t readyCellCount = 0;
    std::uint8_t minResidentAge = std::numeric_limits<std::uint8_t>::max();
    for (std::uint32_t cellIndex = 0; cellIndex < canopyLeafCount; ++cellIndex)
    {
        FoliageCanopyReadyCellInfo cellInfo{};
        if (!m_activeCanopyManager->getReadyCellInfo(canopyLeafIds[cellIndex], cellInfo))
        {
            continue;
        }

        minResidentAge = std::min(minResidentAge, cellInfo.residentFrameAge);
        ++readyCellCount;
    }

    const std::uint32_t minimumReadyCellCount = std::min<std::uint32_t>(
        canopyLeafCount,
        FoliageConfig::kCanopyMinimumReadyCellCount);
    if (readyCellCount < minimumReadyCellCount)
    {
        return false;
    }

    youngestAge = minResidentAge;
    return true;
}

void WorldGridQuadtree::updateCanopyNeighborAgeHintsForNode(std::uint16_t nodeIndex)
{
    if (nodeIndex == QuadtreeNode::NullNodeIndex ||
        nodeIndex >= static_cast<std::uint16_t>(m_nodes.size()))
    {
        return;
    }

    QuadtreeNode& node = m_nodes[nodeIndex];
    node.canopyNeighborResidentAgeHints.fill(255u);

    if (m_activeCanopyManager == nullptr)
    {
        return;
    }

    if (!nodeHasFlag(node, QuadtreeNode::CanopyShouldDrawMask) ||
        std::abs(worldGridQuadtreeLeafSize(node.nodeId) - static_cast<double>(FoliageConfig::kCanopyNodeSizeMeters)) > 0.001)
    {
        return;
    }

    for (std::uint8_t edgeIndex = 0; edgeIndex < 4u; ++edgeIndex)
    {
        const std::uint16_t neighborIndex = findNeighborSubtreeRoot(nodeIndex, edgeIndex);
        if (neighborIndex == QuadtreeNode::NullNodeIndex)
        {
            continue;
        }

        const QuadtreeNode& neighbor = m_nodes[neighborIndex];
        if (std::abs(
            worldGridQuadtreeLeafSize(neighbor.nodeId) -
            worldGridQuadtreeLeafSize(node.nodeId)) > kEdgeCoverageEpsilon)
        {
            continue;
        }

        std::uint8_t neighborYoungestAge = 255u;
        if (!youngestCanopyResidentAgeForNode(neighborIndex, neighborYoungestAge))
        {
            continue;
        }

        node.canopyNeighborResidentAgeHints[edgeIndex] = neighborYoungestAge;
    }
}

std::uint16_t WorldGridQuadtree::findNeighborSubtreeRoot(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const
{
    if (nodeIndex == QuadtreeNode::NullNodeIndex ||
        nodeIndex >= static_cast<std::uint16_t>(m_nodes.size()))
    {
        return QuadtreeNode::NullNodeIndex;
    }

    const QuadtreeNode& node = m_nodes[nodeIndex];
    const std::uint16_t parentIndex = node.parentIndex;
    if (parentIndex == QuadtreeNode::NullNodeIndex)
    {
        std::int64_t neighborGridX = node.nodeId.gridX;
        std::int64_t neighborGridY = node.nodeId.gridY;
        switch (edgeIndex)
        {
        case kEdgeWest:
            --neighborGridX;
            break;
        case kEdgeSouth:
            --neighborGridY;
            break;
        case kEdgeEast:
            ++neighborGridX;
            break;
        case kEdgeNorth:
            ++neighborGridY;
            break;
        default:
            return QuadtreeNode::NullNodeIndex;
        }
        return findBaseNode(neighborGridX, neighborGridY);
    }

    const std::uint8_t quadrant = node.quadrantInParent;
    if (!quadrantTouchesEdge(quadrant, edgeIndex))
    {
        const std::uint16_t siblingQuadrant = adjacentSiblingQuadrant(quadrant, edgeIndex);
        if (siblingQuadrant >= kQuadrantCount)
        {
            return QuadtreeNode::NullNodeIndex;
        }
        return m_nodes[parentIndex].children[siblingQuadrant];
    }

    const std::uint16_t parentNeighborIndex = findNeighborSubtreeRoot(parentIndex, edgeIndex);
    if (parentNeighborIndex == QuadtreeNode::NullNodeIndex)
    {
        return QuadtreeNode::NullNodeIndex;
    }

    const QuadtreeNode& parentNeighbor = m_nodes[parentNeighborIndex];
    if (nodeHasFlag(parentNeighbor, QuadtreeNode::IsLeafMask))
    {
        return parentNeighborIndex;
    }

    const std::uint8_t childQuadrant = descendantQuadrantForNeighbor(quadrant, edgeIndex);
    if (childQuadrant >= kQuadrantCount)
    {
        return QuadtreeNode::NullNodeIndex;
    }
    const std::uint16_t childIndex = parentNeighbor.children[childQuadrant];
    return childIndex;
}

std::uint16_t WorldGridQuadtree::findBaseNode(std::int64_t gridX, std::int64_t gridY) const
{
    const std::int64_t radius = AppConfig::Quadtree::kNeighborRadius;
    const std::int64_t offsetX = gridX - m_baseGridX;
    const std::int64_t offsetY = gridY - m_baseGridY;
    if (offsetX < -radius || offsetX > radius || offsetY < -radius || offsetY > radius)
    {
        return QuadtreeNode::NullNodeIndex;
    }

    const std::size_t sideLength = static_cast<std::size_t>((radius * 2) + 1);
    const std::size_t row = static_cast<std::size_t>(offsetY + radius);
    const std::size_t column = static_cast<std::size_t>(offsetX + radius);
    return m_baseNodes[(row * sideLength) + column];
}

bool WorldGridQuadtree::subtreeEdgeCoveredByTerrain(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const
{
    return subtreeEdgeCoveredBy(
        m_nodes,
        nodeIndex,
        edgeIndex,
        [](const QuadtreeNode& node)
        {
            // Bridge coverage must follow resident terrain surface availability rather than
            // this frame's visibility result. A visible node can still need a bridge against
            // a neighbor whose edge geometry exists but was culled by the node frustum test.
            return WorldGridQuadtree::nodeHasResidentTerrainSurface(node);
        });
}

bool WorldGridQuadtree::subtreeEdgeCoveredByWater(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const
{
    return subtreeEdgeCoveredBy(
        m_nodes,
        nodeIndex,
        edgeIndex,
        [](const QuadtreeNode& node) { return WorldGridQuadtree::nodeHasWaterSurface(node); });
}

bool WorldGridQuadtree::edgeHasDrawableNeighborCoverage(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const
{
    const std::uint16_t neighborRootIndex = findNeighborSubtreeRoot(nodeIndex, edgeIndex);
    if (neighborRootIndex == QuadtreeNode::NullNodeIndex)
    {
        return false;
    }

    const QuadtreeNode& node = m_nodes[nodeIndex];
    const QuadtreeNode& neighborRoot = m_nodes[neighborRootIndex];
    if (worldGridQuadtreeLeafSize(neighborRoot.nodeId) > worldGridQuadtreeLeafSize(node.nodeId) + kEdgeCoverageEpsilon)
    {
        return false;
    }

    return subtreeEdgeCoveredByTerrain(neighborRootIndex, oppositeEdge(edgeIndex));
}

bool WorldGridQuadtree::edgeHasDrawableCoarserNeighbor(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const
{
    const std::uint16_t neighborRootIndex = findNeighborSubtreeRoot(nodeIndex, edgeIndex);
    if (neighborRootIndex == QuadtreeNode::NullNodeIndex)
    {
        return false;
    }

    const QuadtreeNode& node = m_nodes[nodeIndex];
    const QuadtreeNode& neighborRoot = m_nodes[neighborRootIndex];
    const double nodeSize = worldGridQuadtreeLeafSize(node.nodeId);
    const double neighborSize = worldGridQuadtreeLeafSize(neighborRoot.nodeId);
    return
        std::abs(neighborSize - (nodeSize * 2.0)) <= kEdgeCoverageEpsilon &&
        nodeHasResidentTerrainSurface(neighborRoot);
}

bool WorldGridQuadtree::edgeHasEqualLodNeighbor(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const
{
    const std::uint16_t neighborRootIndex = findNeighborSubtreeRoot(nodeIndex, edgeIndex);
    if (neighborRootIndex == QuadtreeNode::NullNodeIndex)
    {
        return false;
    }

    const QuadtreeNode& node = m_nodes[nodeIndex];
    const QuadtreeNode& neighborRoot = m_nodes[neighborRootIndex];
    return std::abs(
        worldGridQuadtreeLeafSize(neighborRoot.nodeId) -
        worldGridQuadtreeLeafSize(node.nodeId)) <= kEdgeCoverageEpsilon;
}

bool WorldGridQuadtree::edgeHasWaterNeighborCoverage(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const
{
    const std::uint16_t neighborRootIndex = findNeighborSubtreeRoot(nodeIndex, edgeIndex);
    if (neighborRootIndex == QuadtreeNode::NullNodeIndex)
    {
        return false;
    }

    const QuadtreeNode& node = m_nodes[nodeIndex];
    const QuadtreeNode& neighborRoot = m_nodes[neighborRootIndex];
    if (worldGridQuadtreeLeafSize(neighborRoot.nodeId) > worldGridQuadtreeLeafSize(node.nodeId) + kEdgeCoverageEpsilon)
    {
        return false;
    }

    return subtreeEdgeCoveredByWater(neighborRootIndex, oppositeEdge(edgeIndex));
}

bool WorldGridQuadtree::edgeHasWaterCoarserNeighbor(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const
{
    const std::uint16_t neighborRootIndex = findNeighborSubtreeRoot(nodeIndex, edgeIndex);
    if (neighborRootIndex == QuadtreeNode::NullNodeIndex)
    {
        return false;
    }

    const QuadtreeNode& node = m_nodes[nodeIndex];
    const QuadtreeNode& neighborRoot = m_nodes[neighborRootIndex];
    const double nodeSize = worldGridQuadtreeLeafSize(node.nodeId);
    const double neighborSize = worldGridQuadtreeLeafSize(neighborRoot.nodeId);
    return
        std::abs(neighborSize - (nodeSize * 2.0)) <= kEdgeCoverageEpsilon &&
        nodeHasWaterSurface(neighborRoot);
}

void WorldGridQuadtree::updateNode(std::uint16_t nodeIndex, const CameraManager::Camera& activeCamera, std::uint64_t frameIndex)
{
    QuadtreeNode& node = m_nodes[nodeIndex];
    if (!nodeHasFlag(node, QuadtreeNode::IsUsedMask))
    {
        return;
    }

    const double size = worldGridQuadtreeLeafSize(node.nodeId);
    const bool nodeUsesCanonicalPages = nodeUsesCanonicalFoliagePages(node);
    const bool nodeIsCanopySize =
        std::abs(size - static_cast<double>(FoliageConfig::kCanopyNodeSizeMeters)) <= 0.001;
    const bool nodeCanUseCanopyFallback =
        size >= static_cast<double>(FoliageConfig::kPageSizeMeters * 2u) &&
        size <= static_cast<double>(FoliageConfig::kCanopyNodeSizeMeters);
    double drawMinHeight = nodeHasFlag(node, QuadtreeNode::HasExtentsMask)
        ? static_cast<double>(node.minHeight)
        : -kVisibilityBoundsHalfHeight;
    double drawMaxHeight = nodeHasFlag(node, QuadtreeNode::HasExtentsMask)
        ? static_cast<double>(node.maxHeight)
        : kVisibilityBoundsHalfHeight;
    if (m_waterVisibilityBoundsEnabled)
    {
        drawMinHeight = std::min(drawMinHeight, static_cast<double>(m_waterVisibilityMinHeight));
        drawMaxHeight = std::max(drawMaxHeight, static_cast<double>(m_waterVisibilityMaxHeight));
    }
    const auto [drawMinCorner, drawMaxCorner] = worldGridQuadtreeLeafBounds(
        node.nodeId,
        drawMinHeight,
        drawMaxHeight);
    const bool shouldDraw = shouldDrawNodeFrustum(
        drawMinCorner,
        drawMaxCorner,
        activeCamera,
        m_viewportExtent);
    setNodeFlag(node, QuadtreeNode::ShouldDrawMask, shouldDraw);
    treeData.maxDepth = std::max(treeData.maxDepth, worldGridQuadtreeLeafDepth(node.nodeId));
    if (shouldDraw)
    {
        ++treeData.drawableNodeCount;
    }

    const bool maintainCanopyResidency =
        shouldDraw &&
        nodeCanUseCanopyFallback &&
        nodeIntersectsCanopyRange(node);
    setNodeFlag(node, QuadtreeNode::MaintainCanopyResidencyMask, maintainCanopyResidency);

    struct TraversalResidencyCache
    {
        bool terrainSliceComputed = false;
        bool hasTerrainSlice = false;
        std::uint16_t terrainSliceIndex = 0;
        bool foliageIdsComputed = false;
        std::uint32_t foliageLeafCount = 0;
        std::array<WorldGridQuadtreeLeafId, 16> foliageLeafIds{};
        bool canopyIdsComputed = false;
        bool hasCanopyLeafIds = false;
        std::uint32_t canopyLeafCount = 0;
        std::array<WorldGridQuadtreeLeafId, FoliageConfig::kCanopyCellCountPerNode> canopyLeafIds{};
    } traversalCache;

    const auto getTerrainSliceIndex = [&]() -> bool
    {
        if (!traversalCache.terrainSliceComputed)
        {
            traversalCache.terrainSliceComputed = true;
            traversalCache.hasTerrainSlice =
                m_heightmapManager.getResidentSliceIndex(node.nodeId, traversalCache.terrainSliceIndex);
        }
        return traversalCache.hasTerrainSlice;
    };

    const auto getFoliageLeafIds = [&]() -> bool
    {
        if (!traversalCache.foliageIdsComputed)
        {
            traversalCache.foliageIdsComputed = true;
            traversalCache.foliageLeafCount = 0;
            collectCanonicalFoliageLeafIds(
                node.nodeId,
                traversalCache.foliageLeafIds,
                traversalCache.foliageLeafCount);
        }
        return traversalCache.foliageLeafCount > 0;
    };

    const auto getCanopyLeafIds = [&]() -> bool
    {
        if (!traversalCache.canopyIdsComputed)
        {
            traversalCache.canopyIdsComputed = true;
            traversalCache.hasCanopyLeafIds = false;
            traversalCache.canopyLeafCount = 0;
            collectCanonicalFoliageLeafIds(
                node.nodeId,
                traversalCache.canopyLeafIds,
                traversalCache.canopyLeafCount);
            traversalCache.hasCanopyLeafIds =
                traversalCache.canopyLeafCount > 0;
        }
        return traversalCache.hasCanopyLeafIds;
    };

    const auto setCanRenderWithoutParentFallback = [&](bool canRenderWithoutParentFallback)
    {
        setNodeFlag(
            node,
            QuadtreeNode::CanRenderWithoutParentFallbackMask,
            canRenderWithoutParentFallback);
    };

    const auto setCanRenderFoliageWithoutCanopyFallback = [&](bool canRenderFoliageWithoutCanopyFallback)
    {
        node.canRenderFoliageWithoutCanopyFallback = canRenderFoliageWithoutCanopyFallback;
    };

    const auto updateNodeFoliageStateForTraversal = [&]()
    {
        if (m_activeFoliageManager == nullptr ||
            !shouldDraw ||
            !nodeHasResidentTerrainSurface(node) ||
            !nodeUsesCanonicalPages)
        {
            setNodeFlag(node, QuadtreeNode::FoliageUploadPendingMask, false);
            setNodeFlag(node, QuadtreeNode::FoliageShouldDrawMask, false);
            return;
        }

        if (!getFoliageLeafIds() || !getTerrainSliceIndex())
        {
            setNodeFlag(node, QuadtreeNode::FoliageUploadPendingMask, false);
            setNodeFlag(node, QuadtreeNode::FoliageShouldDrawMask, false);
            return;
        }

        bool allPagesReady = true;
        for (std::uint32_t pageIndex = 0; pageIndex < traversalCache.foliageLeafCount; ++pageIndex)
        {
            allPagesReady = m_activeFoliageManager->makeResident(
                traversalCache.foliageLeafIds[pageIndex],
                node.nodeId,
                traversalCache.terrainSliceIndex) && allPagesReady;
        }

        setNodeFlag(node, QuadtreeNode::FoliageUploadPendingMask, !allPagesReady);
        setNodeFlag(node, QuadtreeNode::FoliageShouldDrawMask, allPagesReady);
    };

    const auto updateNodeFoliageResidencyHintsForTraversal = [&]()
    {
        if (m_activeFoliageManager == nullptr ||
            !nodeHasFlag(node, QuadtreeNode::IsLeafMask) ||
            shouldDraw ||
            !nodeHasResidentTerrainSurface(node) ||
            !nodeUsesCanonicalPages ||
            !getTerrainSliceIndex() ||
            !getFoliageLeafIds())
        {
            return;
        }

        for (std::uint32_t pageIndex = 0; pageIndex < traversalCache.foliageLeafCount; ++pageIndex)
        {
            (void)m_activeFoliageManager->makeResident(
                traversalCache.foliageLeafIds[pageIndex],
                node.nodeId,
                traversalCache.terrainSliceIndex);
        }
    };

    const auto updateCanopyFlagsForTraversal = [&]()
    {
        bool canopyReady = false;
        bool canopyShouldDraw = false;
        if (m_activeCanopyManager != nullptr && maintainCanopyResidency)
        {
            if (getTerrainSliceIndex() && getCanopyLeafIds())
            {
                canopyReady = true;
                for (std::uint32_t cellIndex = 0; cellIndex < traversalCache.canopyLeafCount; ++cellIndex)
                {
                    canopyReady = m_activeCanopyManager->makeResident(
                        traversalCache.canopyLeafIds[cellIndex],
                        node.nodeId,
                        traversalCache.terrainSliceIndex) && canopyReady;
                }

                if (canopyReady)
                {
                    canopyShouldDraw = nodeIsCanopySize && nodeContributesTerrainDraw(node);
                    if (!canopyShouldDraw && !subtreeCanRenderFoliageWithoutCanopyFallback(nodeIndex))
                    {
                        canopyShouldDraw = true;
                    }
                }
            }
        }

        setNodeFlag(node, QuadtreeNode::CanopyReadyMask, canopyReady);
        setNodeFlag(node, QuadtreeNode::CanopyShouldDrawMask, canopyShouldDraw);
    };

    const auto clearTraversalTransitionFlags = [&]()
    {
        setNodeFlag(node, QuadtreeNode::IsSubdividingMask, false);
        setNodeFlag(node, QuadtreeNode::IsCollapsingMask, false);
        setNodeFlag(node, QuadtreeNode::SubdivisionHandoffMask, false);
        setNodeFlag(node, QuadtreeNode::CollapseHandoffMask, false);
    };

    const auto updateNodeCanopyResidencyHintsForTraversal = [&]()
    {
        if (m_activeCanopyManager == nullptr ||
            !nodeHasFlag(node, QuadtreeNode::IsLeafMask) ||
            shouldDraw ||
            !nodeHasResidentTerrainSurface(node) ||
            !nodeCanUseCanopyFallback ||
            !getTerrainSliceIndex() ||
            !getCanopyLeafIds())
        {
            return;
        }

        for (std::uint32_t cellIndex = 0; cellIndex < traversalCache.canopyLeafCount; ++cellIndex)
        {
            (void)m_activeCanopyManager->makeResident(
                traversalCache.canopyLeafIds[cellIndex],
                node.nodeId,
                traversalCache.terrainSliceIndex);
        }
    };

    const auto finalizeNodeTraversalState = [&](bool recordLeafCount)
    {
        updateNodeFoliageStateForTraversal();
        updateNodeFoliageResidencyHintsForTraversal();
        const bool nodeCanRenderOwnFoliage =
            nodeUsesCanonicalPages &&
            nodeHasResidentTerrainSurface(node) &&
            (m_activeFoliageManager == nullptr ||
                nodeHasFlag(node, QuadtreeNode::FoliageShouldDrawMask));
        if (nodeHasFlag(node, QuadtreeNode::IsLeafMask) || nodeCanRenderOwnFoliage)
        {
            setCanRenderFoliageWithoutCanopyFallback(nodeCanRenderOwnFoliage);
        }
        updateCanopyFlagsForTraversal();
        updateNodeCanopyResidencyHintsForTraversal();
        updateCanopyNeighborAgeHintsForNode(nodeIndex);
        updateNodeNearbyFoliageState(node, frameIndex);
        if (recordLeafCount)
        {
            recordTerrainLeafCount(node);
        }
    };

    const bool wantsChildren =
        size > AppConfig::Quadtree::kMinimumQuadSize &&
        shouldSubdivide(node, activeCamera.position);

    if (wantsChildren)
    {
        ensureChildren(nodeIndex);
        if (nodeHasFlag(node, QuadtreeNode::IsLeafMask))
        {
            const bool resident = m_heightmapManager.makeResident(node.nodeId);
            if (resident && !nodeHasFlag(node, QuadtreeNode::HasExtentsMask))
            {
                applyKnownExtentsToNode(node);
            }
            setNodeFlag(node, QuadtreeNode::IsUploadingMask, !resident);
            clearTraversalTransitionFlags();
            setCanRenderWithoutParentFallback(resident);
            finalizeNodeTraversalState(true);
            return;
        }

        bool allChildrenReady = true;
        bool allChildrenFoliageReady = true;
        for (const std::uint16_t childIndex : node.children)
        {
            if (childIndex == QuadtreeNode::NullNodeIndex)
            {
                allChildrenReady = false;
                allChildrenFoliageReady = false;
                continue;
            }

            updateNode(childIndex, activeCamera, frameIndex);
            allChildrenReady = allChildrenReady &&
                nodeHasFlag(m_nodes[childIndex], QuadtreeNode::CanRenderWithoutParentFallbackMask);
            allChildrenFoliageReady = allChildrenFoliageReady &&
                subtreeCanRenderFoliageWithoutCanopyFallback(childIndex);
        }

        if (!nodeIsParentOfLeaves(m_nodes, node))
        {
            setNodeFlag(node, QuadtreeNode::IsUploadingMask, false);
            clearTraversalTransitionFlags();
            setCanRenderWithoutParentFallback(allChildrenReady);
            setCanRenderFoliageWithoutCanopyFallback(allChildrenFoliageReady);
            finalizeNodeTraversalState(false);
            return;
        }

        const bool wasSubdividing = nodeHasFlag(node, QuadtreeNode::IsSubdividingMask);
        const bool resident = m_heightmapManager.makeResident(node.nodeId);
        if (resident && !nodeHasFlag(node, QuadtreeNode::HasExtentsMask))
        {
            applyKnownExtentsToNode(node);
        }
        setNodeFlag(node, QuadtreeNode::IsUploadingMask, !resident);

        bool isSubdividing = !allChildrenReady;
        if (!isSubdividing && wasSubdividing)
        {
            const bool handoffArmed = nodeHasFlag(node, QuadtreeNode::SubdivisionHandoffMask);
            if (!handoffArmed)
            {
                isSubdividing = true;
                setNodeFlag(node, QuadtreeNode::SubdivisionHandoffMask, true);
            }
            else
            {
                setNodeFlag(node, QuadtreeNode::SubdivisionHandoffMask, false);
            }
        }
        else if (isSubdividing)
        {
            setNodeFlag(node, QuadtreeNode::SubdivisionHandoffMask, false);
        }
        setNodeFlag(node, QuadtreeNode::IsSubdividingMask, isSubdividing);
        setNodeFlag(node, QuadtreeNode::IsCollapsingMask, false);
        setNodeFlag(node, QuadtreeNode::CollapseHandoffMask, false);
        setCanRenderWithoutParentFallback(
            isSubdividing ? resident : allChildrenReady);
        setCanRenderFoliageWithoutCanopyFallback(allChildrenFoliageReady);
        if (isSubdividing && !wasSubdividing)
        {
            ++treeData.subdivisionCountThisFrame;
        }
        finalizeNodeTraversalState(false);
        return;
    }

    if (nodeHasFlag(node, QuadtreeNode::IsLeafMask))
    {
        const bool resident = m_heightmapManager.makeResident(node.nodeId);
        if (resident && !nodeHasFlag(node, QuadtreeNode::HasExtentsMask))
        {
            applyKnownExtentsToNode(node);
        }
        setNodeFlag(node, QuadtreeNode::IsUploadingMask, !resident);
        clearTraversalTransitionFlags();
        setCanRenderWithoutParentFallback(resident);
        finalizeNodeTraversalState(true);
        return;
    }

    bool allChildrenReady = true;
    bool allChildrenFoliageReady = true;
    for (const std::uint16_t childIndex : node.children)
    {
        if (childIndex != QuadtreeNode::NullNodeIndex)
        {
            updateNode(childIndex, activeCamera, frameIndex);
            allChildrenReady = allChildrenReady &&
                nodeHasFlag(m_nodes[childIndex], QuadtreeNode::CanRenderWithoutParentFallbackMask);
            allChildrenFoliageReady = allChildrenFoliageReady &&
                subtreeCanRenderFoliageWithoutCanopyFallback(childIndex);
        }
        else
        {
            allChildrenReady = false;
            allChildrenFoliageReady = false;
        }
    }

    if (!nodeIsParentOfLeaves(m_nodes, node))
    {
        setNodeFlag(node, QuadtreeNode::IsUploadingMask, false);
        clearTraversalTransitionFlags();
        setCanRenderWithoutParentFallback(allChildrenReady);
        setCanRenderFoliageWithoutCanopyFallback(allChildrenFoliageReady);
        finalizeNodeTraversalState(false);
        return;
    }

    const bool wasCollapsing = nodeHasFlag(node, QuadtreeNode::IsCollapsingMask);
    const bool resident = m_heightmapManager.makeResident(node.nodeId);
    if (resident && !nodeHasFlag(node, QuadtreeNode::HasExtentsMask))
    {
        applyKnownExtentsToNode(node);
    }
    setNodeFlag(node, QuadtreeNode::IsUploadingMask, !resident);
    setNodeFlag(node, QuadtreeNode::IsSubdividingMask, false);
    setNodeFlag(node, QuadtreeNode::SubdivisionHandoffMask, false);
    updateCanopyFlagsForTraversal();

    bool keepChildrenAsFallback = !resident;
    if (resident && wasCollapsing)
    {
        const bool handoffArmed = nodeHasFlag(node, QuadtreeNode::CollapseHandoffMask);
        if (!handoffArmed)
        {
            keepChildrenAsFallback = true;
            setNodeFlag(node, QuadtreeNode::CollapseHandoffMask, true);
        }
        else
        {
            setNodeFlag(node, QuadtreeNode::CollapseHandoffMask, false);
        }
    }
    else if (!resident)
    {
        setNodeFlag(node, QuadtreeNode::CollapseHandoffMask, false);
    }

    if (!keepChildrenAsFallback &&
        m_activeCanopyManager != nullptr &&
        maintainCanopyResidency &&
        !nodeHasFlag(node, QuadtreeNode::CanopyReadyMask))
    {
        keepChildrenAsFallback = true;
        setNodeFlag(node, QuadtreeNode::CollapseHandoffMask, false);
    }

    // While collapsing, the children remain drawable until the parent slice is resident,
    // then stay alive for one extra frame so ownership does not switch on the exact handoff frame.
    setNodeFlag(node, QuadtreeNode::IsCollapsingMask, keepChildrenAsFallback);

    if (resident && !keepChildrenAsFallback)
    {
        for (std::uint16_t& childIndex : node.children)
        {
            if (childIndex != QuadtreeNode::NullNodeIndex)
            {
                freeSubtree(childIndex);
                childIndex = QuadtreeNode::NullNodeIndex;
            }
        }
        setNodeFlag(node, QuadtreeNode::IsLeafMask, true);
        setNodeFlag(node, QuadtreeNode::IsUploadingMask, false);
        setCanRenderWithoutParentFallback(true);
        finalizeNodeTraversalState(true);
        return;
    }

    if (!wasCollapsing)
    {
        ++treeData.collapseCountThisFrame;
    }
    setCanRenderWithoutParentFallback(allChildrenReady);
    setCanRenderFoliageWithoutCanopyFallback(allChildrenFoliageReady);
    finalizeNodeTraversalState(false);
}

bool WorldGridQuadtree::nodeHasFlag(const QuadtreeNode& node, std::uint16_t mask)
{
    return (node.flags & mask) != 0;
}

void WorldGridQuadtree::setNodeFlag(QuadtreeNode& node, std::uint16_t mask, bool enabled)
{
    if (enabled)
    {
        node.flags |= mask;
    }
    else
    {
        node.flags &= static_cast<std::uint16_t>(~mask);
    }
}

bool WorldGridQuadtree::shouldSubdivide(const QuadtreeNode& node, const Position& cameraPosition) const
{
    const double size = worldGridQuadtreeLeafSize(node.nodeId);
    const glm::dvec3 cameraWorld = cameraPosition.worldPosition();
    const double subdivisionDistance = size * AppConfig::Quadtree::kSubdivisionDistanceFactor;

    if (nodeHasFlag(node, QuadtreeNode::HasExtentsMask))
    {
        double minHeight = static_cast<double>(node.minHeight);
        double maxHeight = static_cast<double>(node.maxHeight);
        if (m_waterVisibilityBoundsEnabled)
        {
            minHeight = std::min(minHeight, static_cast<double>(m_waterVisibilityMinHeight));
            maxHeight = std::max(maxHeight, static_cast<double>(m_waterVisibilityMaxHeight));
        }
        const auto [minCorner, maxCorner] = worldGridQuadtreeLeafBounds(
            node.nodeId,
            minHeight,
            maxHeight);
        const glm::dvec3 minWorld = minCorner.worldPosition();
        const glm::dvec3 maxWorld = maxCorner.worldPosition();
        return distanceSquaredToBounds(
            cameraWorld.x,
            cameraWorld.y,
            cameraWorld.z,
            minWorld.x,
            minWorld.y,
            minWorld.z,
            maxWorld.x,
            maxWorld.y,
            maxWorld.z) <= subdivisionDistance * subdivisionDistance;
    }

    const auto [minCorner, maxCorner] = worldGridQuadtreeLeafBounds(node.nodeId);
    const glm::dvec3 nodeCenter = nodeCenterAtZeroHeight(minCorner, maxCorner);
    const glm::dvec3 toCenter = cameraWorld - nodeCenter;
    return glm::dot(toCenter, toCenter) <= subdivisionDistance * subdivisionDistance;
}

double WorldGridQuadtree::distanceSquaredToBounds(
    double pointX,
    double pointY,
    double pointZ,
    double minX,
    double minY,
    double minZ,
    double maxX,
    double maxY,
    double maxZ)
{
    const double deltaX = pointX < minX ? minX - pointX : (pointX > maxX ? pointX - maxX : 0.0);
    const double deltaY = pointY < minY ? minY - pointY : (pointY > maxY ? pointY - maxY : 0.0);
    const double deltaZ = pointZ < minZ ? minZ - pointZ : (pointZ > maxZ ? pointZ - maxZ : 0.0);
    return (deltaX * deltaX) + (deltaY * deltaY) + (deltaZ * deltaZ);
}
