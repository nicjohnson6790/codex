#include "WorldGridQuadtree.hpp"

#include "FoliageCanopyRenderer.hpp"
#include "FoliageImposterRenderer.hpp"
#include "NearbyFoliageRenderer.hpp"
#include "PerformanceCapture.hpp"
#include "QuadtreeMeshRenderer.hpp"
#include "RenderEngines.hpp"
#include "WorldGridFoliageCanopyManager.hpp"
#include "WorldGridFoliageManager.hpp"
#include "WorldGridNearbyFoliageManager.hpp"
#include "WorldGridQuadtreeWaterManager.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <glm/geometric.hpp>
#include <limits>
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

void initializeBaseNode(QuadtreeNode& node, std::int64_t gridX, std::int64_t gridY)
{
    node.nodeId = {
        .gridX = gridX,
        .gridY = gridY,
        .subdivisionPath = 0,
    };
    node.parentIndex = QuadtreeNode::NullNodeIndex;
    node.children.fill(QuadtreeNode::NullNodeIndex);
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

WorldGridVisibilityFrustum buildVisibilityFrustum(
    const Position& cameraPosition,
    const glm::dvec3& cameraForward,
    const glm::dvec3& cameraUp,
    Extent2D viewportExtent)
{
    const glm::dvec3 forward = glm::normalize(cameraForward);
    glm::dvec3 right = glm::cross(forward, cameraUp);
    if (glm::length(right) <= 0.00001)
    {
        right = { 1.0, 0.0, 0.0 };
    }
    right = glm::normalize(right);
    const glm::dvec3 up = glm::normalize(glm::cross(right, forward));

    const double aspectRatio =
        static_cast<double>(std::max(viewportExtent.width, 1u)) /
        static_cast<double>(std::max(viewportExtent.height, 1u));
    const double tanHalfVerticalFov = std::tan(AppConfig::Camera::kVerticalFovRadians * 0.5);
    const double tanHalfHorizontalFov = tanHalfVerticalFov * aspectRatio;

    WorldGridVisibilityFrustum frustum{};
    frustum.cameraPosition = cameraPosition;
    frustum.planeNormals = {
        forward,
        right + (forward * tanHalfHorizontalFov),
        (-right) + (forward * tanHalfHorizontalFov),
        up + (forward * tanHalfVerticalFov),
        (-up) + (forward * tanHalfVerticalFov),
    };
    frustum.planeOffsets = {
        -AppConfig::Camera::kNearPlane,
        0.0,
        0.0,
        0.0,
        0.0,
    };
    return frustum;
}

bool shouldDrawNodeFrustum(
    const Position& minCornerPosition,
    const Position& maxCornerPosition,
    const WorldGridVisibilityFrustum& frustum)
{
    const glm::dvec3 minCorner = minCornerPosition.localCoordinatesInCellOf(frustum.cameraPosition);
    const glm::dvec3 maxCorner = maxCornerPosition.localCoordinatesInCellOf(frustum.cameraPosition);
    const glm::dvec3 center = (minCorner + maxCorner) * 0.5;
    const glm::dvec3 extents = (maxCorner - minCorner) * 0.5;

    const auto boxIsOutsidePlane = [&center, &extents](const glm::dvec3& planeNormal, double planeOffset)
    {
        const double projectedRadius =
            std::abs(planeNormal.x) * extents.x +
            std::abs(planeNormal.y) * extents.y +
            std::abs(planeNormal.z) * extents.z;
        const double signedDistance = glm::dot(planeNormal, center) + planeOffset;
        return (signedDistance + projectedRadius) < 0.0;
    };

    for (std::size_t planeIndex = 0; planeIndex < frustum.planeNormals.size(); ++planeIndex)
    {
        if (boxIsOutsidePlane(frustum.planeNormals[planeIndex], frustum.planeOffsets[planeIndex]))
        {
            return false;
        }
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
        return { 0u, 1u };
    }
}

bool quadtreeNodeHasChildren(const QuadtreeNode& node)
{
    return std::any_of(
        node.children.begin(),
        node.children.end(),
        [](std::uint16_t childIndex) { return childIndex != QuadtreeNode::NullNodeIndex; });
}

template <typename Predicate>
bool subtreeEdgeCoveredBy(
    const std::array<QuadtreeNode, WorldGridQuadtree::kNodeCapacity>& nodes,
    std::uint16_t nodeIndex,
    std::uint8_t edgeIndex,
    Predicate&& predicate)
{
    if (nodeIndex == QuadtreeNode::NullNodeIndex)
    {
        return false;
    }

    const QuadtreeNode& node = nodes[nodeIndex];
    if (!quadtreeNodeHasChildren(node))
    {
        return predicate(nodeIndex);
    }

    for (std::uint8_t quadrant : edgeChildQuadrants(edgeIndex))
    {
        const std::uint16_t childIndex = node.children[quadrant];
        if (childIndex == QuadtreeNode::NullNodeIndex ||
            !subtreeEdgeCoveredBy(nodes, childIndex, edgeIndex, predicate))
        {
            return false;
        }
    }
    return true;
}

template <std::size_t Capacity>
void collectCanonicalLeafIds(
    const WorldGridQuadtreeLeafId& nodeId,
    std::array<WorldGridQuadtreeLeafId, Capacity>& canonicalLeafIds,
    std::uint32_t maxPagesPerSide,
    std::uint32_t& canonicalLeafCount)
{
    canonicalLeafCount = 0;
    const double nodeSize = worldGridQuadtreeLeafSize(nodeId);
    if (nodeSize < static_cast<double>(FoliageConfig::kPageSizeMeters))
    {
        return;
    }

    const std::uint32_t pageCountPerSide =
        static_cast<std::uint32_t>(std::llround(nodeSize / static_cast<double>(FoliageConfig::kPageSizeMeters)));
    if (pageCountPerSide == 0u ||
        pageCountPerSide > maxPagesPerSide ||
        (pageCountPerSide * pageCountPerSide) > canonicalLeafIds.size())
    {
        return;
    }

    for (std::uint32_t pageZ = 0; pageZ < pageCountPerSide; ++pageZ)
    {
        for (std::uint32_t pageX = 0; pageX < pageCountPerSide; ++pageX)
        {
            WorldGridQuadtreeLeafId canonicalLeafId{
                .gridX = nodeId.gridX,
                .gridY = nodeId.gridY,
                .subdivisionPath = nodeId.subdivisionPath,
            };

            std::uint32_t localTileX = pageX;
            std::uint32_t localTileZ = pageZ;
            std::uint32_t currentTilesPerSide = pageCountPerSide;
            while (currentTilesPerSide > 1u)
            {
                const std::uint32_t halfTilesPerSide = currentTilesPerSide / 2u;
                const bool east = localTileX >= halfTilesPerSide;
                const bool north = localTileZ >= halfTilesPerSide;
                const std::uint32_t quadrant = east ? (north ? 0u : 2u) : (north ? 1u : 3u);
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

void collectCanonicalFoliageLeafIds(
    const WorldGridQuadtreeLeafId& nodeId,
    std::array<WorldGridQuadtreeLeafId, 16>& canonicalLeafIds,
    std::uint32_t& canonicalLeafCount)
{
    collectCanonicalLeafIds(nodeId, canonicalLeafIds, 4u, canonicalLeafCount);
}

void collectCanonicalCanopyLeafIds(
    const WorldGridQuadtreeLeafId& nodeId,
    std::array<WorldGridQuadtreeLeafId, FoliageConfig::kCanopyCellCountPerNode>& canonicalLeafIds,
    std::uint32_t& canonicalLeafCount)
{
    collectCanonicalLeafIds(
        nodeId,
        canonicalLeafIds,
        FoliageConfig::kCanopyCellsPerSide,
        canonicalLeafCount);
}
}

WorldGridQuadtree::WorldGridQuadtree()
{
    reset();
}

void WorldGridQuadtree::updateTree(
    const CameraManager::Camera& activeCamera,
    Extent2D viewportExtent,
    QuadtreeMeshRenderer& meshRenderer,
    std::uint64_t frameIndex)
{
    HELLO_PROFILE_SCOPE("WorldGridQuadtree::UpdateTree");
    m_currentFrameIndex = frameIndex;

    m_heightmapManager.collectCompletedCpuReadbacks(meshRenderer);

    std::vector<QuadtreeMeshRenderer::GeneratedHeightmapExtents> completedExtents;
    meshRenderer.collectCompletedHeightmapExtents(completedExtents);
    for (const QuadtreeMeshRenderer::GeneratedHeightmapExtents& generatedExtents : completedExtents)
    {
        m_heightmapManager.applyGeneratedExtents(
            generatedExtents.leafId,
            generatedExtents.sliceIndex,
            generatedExtents.extents,
            generatedExtents.job);
    }

    m_activeCameraPosition = activeCamera.position;
    m_activeCameraForward = activeCamera.forward;
    m_activeCameraUp = activeCamera.up;
    m_viewportExtent = viewportExtent;
    const glm::dvec3 cameraWorld = activeCamera.position.worldPosition();
    m_nearbyCameraPageX = static_cast<std::int64_t>(
        std::floor(cameraWorld.x / static_cast<double>(FoliageConfig::kPageSizeMeters)));
    m_nearbyCameraPageZ = static_cast<std::int64_t>(
        std::floor(cameraWorld.z / static_cast<double>(FoliageConfig::kPageSizeMeters)));

    m_heightmapManager.ageMap();
    treeData = {};
    refreshBaseNodes(activeCamera.position);

    for (const std::uint16_t nodeIndex : m_baseNodes)
    {
        if (nodeIndex != QuadtreeNode::NullNodeIndex)
        {
            updateNode(nodeIndex, activeCamera);
        }
    }

    m_heightmapManager.scheduleQueuedGenerations(meshRenderer);
}

void WorldGridQuadtree::emitSceneDraws(
    RenderEngines& renderEngines,
    WorldGridFoliageManager* foliageManager,
    WorldGridFoliageCanopyManager* canopyManager,
    FoliageImposterRenderer* foliageRenderer,
    WorldGridNearbyFoliageManager* nearbyFoliageManager,
    NearbyFoliageRenderer* nearbyFoliageRenderer,
    FoliageCanopyRenderer* canopyRenderer,
    WorldGridQuadtreeWaterManager* waterManager)
{
    HELLO_PROFILE_SCOPE("WorldGridQuadtree::EmitSceneDraws");

    const std::uint16_t warmupSlot = static_cast<std::uint16_t>(m_currentFrameIndex % 16u);
    const WorldGridVisibilityFrustum visibilityFrustum = buildVisibilityFrustum(
        m_activeCameraPosition,
        m_activeCameraForward,
        m_activeCameraUp,
        m_viewportExtent);
    const auto warmResidencyForNode = [&](
        std::uint16_t nodeIndex,
        const QuadtreeNode& node,
        const HeightmapExtents* extents)
    {
        const CacheIndex terrainSliceIndex = residentHeightmap(nodeIndex);
        if (terrainSliceIndex == kUnavailableCacheIndex)
        {
            return;
        }

        bool foliageReady = false;
        auto& hints = m_residencyHints[nodeIndex];
        if (canopyManager != nullptr && nodeCanUseCanopy(node))
        {
            const CanopyCanonicalCellView cells = canopyCellIdsForNode(nodeIndex, node);
            if (cells.cellCount > 0u)
            {
                for (std::uint32_t cellIndex = 0; cellIndex < cells.cellCount; ++cellIndex)
                {
                    auto& hint = hints.canopyCells[cellIndex];
                    hint = canopyManager->requestAsset(cells.cellIds[cellIndex], node.nodeId, terrainSliceIndex, hint);
                }
            }
        }
        else if (foliageManager != nullptr && nodeUsesCanonicalFoliagePages(node))
        {
            const FoliageCanonicalPageView pages = foliagePageIdsForNode(nodeIndex, node);
            if (pages.pageCount > 0u)
            {
                foliageReady = true;
                for (std::uint32_t pageIndex = 0; pageIndex < pages.pageCount; ++pageIndex)
                {
                    auto& hint = hints.foliagePages[pageIndex];
                    hint = foliageManager->requestAsset(
                        pages.pageIds[pageIndex], node.nodeId, terrainSliceIndex, hint);
                    foliageReady = hint != kUnavailableCacheIndex && foliageReady;
                }
            }
        }

        if (canopyManager != nullptr && !foliageReady && nodeCanUseCanopyFallback(node))
        {
            const CanopyCanonicalCellView cells = canopyCellIdsForNode(nodeIndex, node);
            if (cells.cellCount > 0u)
            {
                for (std::uint32_t cellIndex = 0; cellIndex < cells.cellCount; ++cellIndex)
                {
                    auto& hint = hints.canopyCells[cellIndex];
                    hint = canopyManager->requestAsset(cells.cellIds[cellIndex], node.nodeId, terrainSliceIndex, hint);
                }
            }
        }

        if (foliageManager != nullptr &&
            nearbyFoliageRenderer != nullptr &&
            extents != nullptr &&
            nodeIsInNearbyFoliageTopology(node) &&
            nodeIntersectsNearbyFoliageRange(node, *extents))
        {
            // Nearby topology is exactly one canonical page: entry zero is this node.
            auto& residentIndex = hints.foliagePages[0];
            if (!foliageReady)
                residentIndex = foliageManager->requestAsset(node.nodeId, node.nodeId, terrainSliceIndex, residentIndex);
            if (residentIndex != kUnavailableCacheIndex)
            {
                FoliageReadyPageInfo pageInfo{};
                if (foliageManager->buildReadyPageInfo(node.nodeId, residentIndex, pageInfo))
                {
                    if (nearbyFoliageManager != nullptr)
                        hints.nearbyFoliage = nearbyFoliageManager->requestAsset(
                            node.nodeId, pageInfo, *nearbyFoliageRenderer, hints.nearbyFoliage);
                }
            }
        }
    };

    for (std::uint16_t nodeIndex = 0; nodeIndex < static_cast<std::uint16_t>(m_nodes.size()); ++nodeIndex)
    {
        const QuadtreeNode& node = m_nodes[nodeIndex];
        if (!nodeOccupied(nodeIndex) || nodeHasChildren(node))
        {
            continue;
        }

        HeightmapExtents extents{};
        const bool hasExtents = m_heightmapManager.buildExtents(node.nodeId, residentHeightmap(nodeIndex), extents);
        if (!nodeIsVisible(node, visibilityFrustum, hasExtents ? &extents : nullptr))
        {
            if ((nodeIndex % 16u) == warmupSlot)
            {
                warmResidencyForNode(nodeIndex, node, hasExtents ? &extents : nullptr);
            }
            continue;
        }

        const CacheIndex terrainSliceIndex = residentHeightmap(nodeIndex);
        const bool hasTerrainSlice = terrainSliceIndex != kUnavailableCacheIndex;

        bool foliageReady = false;
        auto& hints = m_residencyHints[nodeIndex];
        auto& foliageResidentIndices = hints.foliagePages;
        FoliageCanonicalPageView foliagePages{};
        if (hasTerrainSlice && foliageManager != nullptr && nodeUsesCanonicalFoliagePages(node))
        {
            foliagePages = foliagePageIdsForNode(nodeIndex, node);
            if (foliagePages.pageCount > 0u)
            {
                foliageReady = true;
                for (std::uint32_t pageIndex = 0; pageIndex < foliagePages.pageCount; ++pageIndex)
                {
                    foliageResidentIndices[pageIndex] = foliageManager->requestAsset(
                        foliagePages.pageIds[pageIndex],
                        node.nodeId,
                        terrainSliceIndex,
                        foliageResidentIndices[pageIndex]);
                    foliageReady =
                        foliageResidentIndices[pageIndex] != kUnavailableCacheIndex &&
                        foliageReady;
                }
            }
        }

        const bool drawCanopy =
            nodeCanUseCanopy(node) ||
            (!foliageReady && nodeCanUseCanopyFallback(node));
        bool canopyReady = false;
        auto& canopyResidentIndices = hints.canopyCells;
        CanopyCanonicalCellView canopyCells{};
        std::uint32_t readyCanopyCellCount = 0;
        if (hasTerrainSlice && canopyManager != nullptr && drawCanopy)
        {
            canopyCells = canopyCellIdsForNode(nodeIndex, node);
            if (canopyCells.cellCount > 0u)
            {
                canopyReady = true;
                for (std::uint32_t cellIndex = 0; cellIndex < canopyCells.cellCount; ++cellIndex)
                {
                    canopyResidentIndices[cellIndex] = canopyManager->requestAsset(
                        canopyCells.cellIds[cellIndex],
                        node.nodeId,
                        terrainSliceIndex,
                        canopyResidentIndices[cellIndex]);
                    if (canopyResidentIndices[cellIndex] != kUnavailableCacheIndex)
                    {
                        ++readyCanopyCellCount;
                    }
                    canopyReady =
                        canopyResidentIndices[cellIndex] != kUnavailableCacheIndex &&
                        canopyReady;
                }
            }
        }

        bool nearbyReady = false;
        if (hasTerrainSlice &&
            foliageManager != nullptr &&
            nearbyFoliageRenderer != nullptr &&
            nodeIsInNearbyFoliageTopology(node) &&
            hasExtents &&
            nodeIntersectsNearbyFoliageRange(node, extents))
        {
            // Nearby topology is exactly one canonical page: entry zero is this node.
            auto& nearbyFoliageResidentIndex = hints.foliagePages[0];
            if (!foliageReady)
                nearbyFoliageResidentIndex = foliageManager->requestAsset(
                    node.nodeId, node.nodeId, terrainSliceIndex, nearbyFoliageResidentIndex);
            if (nearbyFoliageResidentIndex != kUnavailableCacheIndex)
            {
                FoliageReadyPageInfo pageInfo{};
                if (foliageManager->buildReadyPageInfo(node.nodeId, nearbyFoliageResidentIndex, pageInfo))
                {
                    if (nearbyFoliageManager != nullptr)
                    {
                        hints.nearbyFoliage = nearbyFoliageManager->requestAsset(
                            node.nodeId, pageInfo, *nearbyFoliageRenderer, hints.nearbyFoliage);
                        nearbyReady = hints.nearbyFoliage != kUnavailableCacheIndex;
                    }
                }
            }
        }

        ++treeData.drawableNodeCount;

        if (renderEngines.quadtreeMeshRenderer != nullptr)
        {
            emitTerrainDrawForNode(nodeIndex, node, renderEngines);
        }

        bool canopyDrawn = false;
        const std::uint32_t minimumReadyCanopyCellCount = std::min<std::uint32_t>(
            canopyCells.cellCount,
            FoliageConfig::kCanopyMinimumReadyCellCount);
        const bool canopyDrawable =
            canopyReady &&
            canopyCells.cellCount > 0u &&
            readyCanopyCellCount >= minimumReadyCanopyCellCount;
        if (canopyDrawable && canopyManager != nullptr && canopyRenderer != nullptr && drawCanopy)
        {
            std::array<std::uint8_t, 4> edgeFadeStrengths{};
            if (nodeIsCanopyLod(node))
            {
                for (std::uint8_t edgeIndex = 0; edgeIndex < edgeFadeStrengths.size(); ++edgeIndex)
                {
                    edgeFadeStrengths[edgeIndex] = canopyEdgeFadeStrength(nodeIndex, edgeIndex);
                }
            }
            canopyManager->emitCanopyDraw(
                node.nodeId,
                terrainSliceIndex,
                canopyCells.cellIds,
                canopyResidentIndices,
                canopyCells.cellCount,
                readyCanopyCellCount,
                nodeIsCanopyLod(node) ? nodeStateFadeAgeFrames(node) : FoliageConfig::kCanopyFadeInFrameCount,
                edgeFadeStrengths,
                *canopyRenderer);
            canopyDrawn = true;
        }

        if (!canopyDrawn && foliageReady && foliageManager != nullptr && foliageRenderer != nullptr)
        {
            for (std::uint32_t pageIndex = 0; pageIndex < foliagePages.pageCount; ++pageIndex)
            {
                (void)foliageManager->emitPageDraw(
                    foliagePages.pageIds[pageIndex],
                    foliageResidentIndices[pageIndex],
                    node.nodeId,
                    terrainSliceIndex,
                    *foliageRenderer);
            }
        }

        if (nearbyFoliageRenderer != nullptr &&
            nearbyReady)
        {
            nearbyFoliageRenderer->addNearbyInstancesForPage(
                node.nodeId,
                hints.nearbyFoliage,
                terrainSliceIndex,
                m_activeCameraPosition,
                FoliageConfig::kNearbyDefaultRadiusMeters);
        }

        if (waterManager != nullptr)
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
    const CacheIndex sliceIndex = residentHeightmap(nodeIndex);
    if (sliceIndex == kUnavailableCacheIndex)
    {
        return;
    }

    const std::uint8_t scalePow = worldGridQuadtreeLeafScalePow(node.nodeId);
    if (scalePow < treeData.terrainDrawCountByScalePow.size())
    {
        ++treeData.terrainDrawCountByScalePow[scalePow];
    }

    auto& hint = m_residencyHints[nodeIndex].heightmap;
    hint = m_heightmapManager.requestAsset(node.nodeId, hint);
    if (hint != kUnavailableCacheIndex)
        renderEngines.quadtreeMeshRenderer->addLeaf(node.nodeId, hint);

    std::array<std::optional<CoarseTerrainNeighbor>, 4> coarseNeighbors{};
    for (std::uint8_t edgeIndex = 0; edgeIndex < 4u; ++edgeIndex)
        coarseNeighbors[edgeIndex] = drawableCoarserNeighbor(nodeIndex, edgeIndex);

    struct CornerOwner
    {
        WorldGridQuadtreeLeafId leafId{};
        std::uint16_t sliceIndex = 0;
        bool coarse = false;
    };
    std::array<CornerOwner, 4> corners{}; // southwest, southeast, northeast, northwest
    for (auto &corner : corners)
        corner = {node.nodeId, sliceIndex, false};
    constexpr std::array<std::array<std::uint8_t, 2>, 4> edgeCorners{{{{0, 3}}, {{1, 0}}, {{2, 1}}, {{3, 2}}}};
    for (std::uint8_t edgeIndex = 0; edgeIndex < 4u; ++edgeIndex)
        if (coarseNeighbors[edgeIndex])
            for (const std::uint8_t cornerIndex : edgeCorners[edgeIndex])
                if (!corners[cornerIndex].coarse)
                    corners[cornerIndex] = {m_nodes[coarseNeighbors[edgeIndex]->nodeIndex].nodeId,
                                            coarseNeighbors[edgeIndex]->sliceIndex, true};

    const auto [nodeMin, nodeMax] = worldGridQuadtreeLeafBounds(node.nodeId);
    const std::array<Position, 4> cornerPositions{{nodeMin,
                                                   Position(nodeMax.gridX(), nodeMin.gridY(),
                                                            {nodeMax.localPosition().x, 0.0, nodeMin.localPosition().z}),
                                                   nodeMax,
                                                   Position(nodeMin.gridX(), nodeMax.gridY(),
                                                            {nodeMin.localPosition().x, 0.0, nodeMax.localPosition().z})}};
    const auto packCornerSample = [&](std::uint8_t cornerIndex) {
        const auto [ownerMin, ownerMax] = worldGridQuadtreeLeafBounds(corners[cornerIndex].leafId);
        (void)ownerMax;
        const glm::dvec3 offset = cornerPositions[cornerIndex].localCoordinatesInCellOf(ownerMin);
        const double pitch = worldGridQuadtreeLeafSize(corners[cornerIndex].leafId) /
                             AppConfig::Terrain::kHeightmapLeafIntervalCount;
        const std::uint32_t x = static_cast<std::uint32_t>(std::llround(offset.x / pitch)) + AppConfig::Terrain::kHeightmapLeafHalo;
        const std::uint32_t z = static_cast<std::uint32_t>(std::llround(offset.z / pitch)) + AppConfig::Terrain::kHeightmapLeafHalo;
        return x | (z << 16u);
    };

    for (std::uint8_t edgeIndex = 0; edgeIndex < 4u; ++edgeIndex)
    {
        const auto cornerIds = edgeCorners[edgeIndex];
        QuadtreeMeshRenderer::BridgeHeightmaps heightmaps{
            sliceIndex,
            coarseNeighbors[edgeIndex] ? coarseNeighbors[edgeIndex]->sliceIndex : sliceIndex,
            corners[cornerIds[0]].sliceIndex,
            corners[cornerIds[1]].sliceIndex,
            packCornerSample(cornerIds[0]),
            packCornerSample(cornerIds[1]),
            coarseNeighbors[edgeIndex] ? coarseNeighbors[edgeIndex]->half : 0u,
        };
        if (coarseNeighbors[edgeIndex])
        {
            renderEngines.quadtreeMeshRenderer->addCoarseBridge(node.nodeId, heightmaps, edgeIndex);
        }
        else
        {
            renderEngines.quadtreeMeshRenderer->addBridge(node.nodeId, heightmaps, edgeIndex);
        }
    }
}

void WorldGridQuadtree::clearTerrainCache()
{
    m_heightmapManager.clearCache();
    reset();
}

WorldGridQuadtree::FoliageCanonicalPageView WorldGridQuadtree::foliagePageIdsForNode(
    std::uint16_t nodeIndex,
    const QuadtreeNode& node) const
{
    FoliageCanonicalPageCache& cache = m_foliageCanonicalPageCaches[nodeIndex];
    if (!cache.valid || !(cache.nodeId == node.nodeId))
    {
        collectCanonicalFoliageLeafIds(node.nodeId, cache.pageIds, cache.pageCount);
        cache.nodeId = node.nodeId;
        cache.valid = true;
    }

    return {
        .pageIds = cache.pageIds.data(),
        .pageCount = cache.pageCount,
    };
}

WorldGridQuadtree::CanopyCanonicalCellView WorldGridQuadtree::canopyCellIdsForNode(
    std::uint16_t nodeIndex,
    const QuadtreeNode& node) const
{
    CanopyCanonicalCellCache& cache = m_canopyCanonicalCellCaches[nodeIndex];
    if (!cache.valid || !(cache.nodeId == node.nodeId))
    {
        collectCanonicalCanopyLeafIds(node.nodeId, cache.cellIds, cache.cellCount);
        cache.nodeId = node.nodeId;
        cache.valid = true;
    }

    return {
        .cellIds = cache.cellIds.data(),
        .cellCount = cache.cellCount,
    };
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
        const std::uint16_t nodeIndex = static_cast<std::uint16_t>(&node - m_nodes.data());
        if (!nodeOccupied(nodeIndex) || nodeHasChildren(node))
        {
            continue;
        }

        HeightmapExtents extents{};
        const bool hasExtents = m_heightmapManager.buildExtents(node.nodeId, residentHeightmap(nodeIndex), extents);
        m_debugRenderer.appendNodeBorder(
            renderEngines,
            node.nodeId,
            treeData.maxDepth,
            hasExtents,
            extents.minHeight,
            extents.maxHeight);
    }
}

void WorldGridQuadtree::emitWaterDrawForNode(
    std::uint16_t nodeIndex,
    const QuadtreeNode& node,
    WorldGridQuadtreeWaterManager& waterManager) const
{
    const auto [minCorner, maxCorner] = worldGridQuadtreeLeafBounds(node.nodeId);
    (void)maxCorner;
    const double leafSizeMeters = worldGridQuadtreeLeafSize(node.nodeId);
    const std::uint8_t quadtreeLodHint = std::min<std::uint8_t>(worldGridQuadtreeLeafScalePow(node.nodeId), 4u);
    const CacheIndex terrainSliceIndex = residentHeightmap(nodeIndex);
    const bool hasTerrainSlice = terrainSliceIndex != kUnavailableCacheIndex;
    HeightmapExtents extents{};
    const bool hasExtents = m_heightmapManager.buildExtents(node.nodeId, terrainSliceIndex, extents);
    const bool queuedLeaf = waterManager.requestLeaf(
        node.nodeId,
        minCorner,
        leafSizeMeters,
        hasExtents,
        extents.minHeight,
        hasTerrainSlice,
        hasTerrainSlice ? terrainSliceIndex : 0,
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

CacheIndex WorldGridQuadtree::residentHeightmap(std::uint16_t nodeIndex) const
{
    auto& hint = m_residencyHints[nodeIndex].heightmap;
    hint = m_heightmapManager.isResident(m_nodes[nodeIndex].nodeId, hint);
    return hint;
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

            if (m_hasBaseGrid)
            {
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
            }

            if (newBaseNodes[baseNodeArrayIndex] == QuadtreeNode::NullNodeIndex)
            {
                WorldGridQuadtreeLeafId baseId{
                    .gridX = targetGridX,
                    .gridY = targetGridY,
                    .subdivisionPath = 0,
                };
                if (m_heightmapManager.requestAsset(baseId) != WorldGridQuadtreeHeightmapManager::kUnavailable)
                {
                    const std::uint16_t nodeIndex = allocateNode();
                    if (nodeIndex != QuadtreeNode::NullNodeIndex)
                    {
                        initializeBaseNode(m_nodes[nodeIndex], targetGridX, targetGridY);
                        newBaseNodes[baseNodeArrayIndex] = nodeIndex;
                    }
                }
            }

            ++baseNodeArrayIndex;
        }
    }

    if (m_hasBaseGrid)
    {
        for (std::size_t oldIndex = 0; oldIndex < oldBaseNodes.size(); ++oldIndex)
        {
            const std::uint16_t nodeIndex = oldBaseNodes[oldIndex];
            if (nodeIndex != QuadtreeNode::NullNodeIndex && !reusedOldBaseNodes[oldIndex])
            {
                freeSubtree(nodeIndex);
            }
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
    m_residencyHints[nodeIndex] = {};
    m_nodes[nodeIndex].parentIndex = QuadtreeNode::NullNodeIndex;
    m_nodes[nodeIndex].currentStateStartFrame = m_currentFrameIndex;
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

void WorldGridQuadtree::ensureChildren(
    std::uint16_t nodeIndex,
    const std::array<WorldGridQuadtreeLeafId, 4>& childIds)
{
    QuadtreeNode& node = m_nodes[nodeIndex];
    if (nodeHasChildren(node))
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
        child.nodeId = childIds[quadrant];
        child.parentIndex = nodeIndex;
        child.currentStateStartFrame = m_currentFrameIndex;
        child.children.fill(QuadtreeNode::NullNodeIndex);
        childIndices[quadrant] = childIndex;
    }

    node.children = childIndices;
    node.currentStateStartFrame = m_currentFrameIndex;
    ++treeData.subdivisionCountThisFrame;
}

void WorldGridQuadtree::updateNode(std::uint16_t nodeIndex, const CameraManager::Camera& activeCamera)
{
    if (!nodeOccupied(nodeIndex))
    {
        return;
    }

    QuadtreeNode& node = m_nodes[nodeIndex];
    treeData.maxDepth = std::max(treeData.maxDepth, worldGridQuadtreeLeafDepth(node.nodeId));
    const LodDecision decision = evaluateLodPolicy(nodeIndex, activeCamera);
    const bool hasChildren = nodeHasChildren(node);

    if (!hasChildren)
    {
        auto& hint = m_residencyHints[nodeIndex].heightmap;
        hint = m_heightmapManager.requestAsset(node.nodeId, hint);
        recordTerrainLeafCount(node);
    }

    if (decision == LodDecision::Subdivide)
    {
        const std::array<WorldGridQuadtreeLeafId, 4> childIds = childIdsForNode(node);
        bool allChildrenResident = true;
        for (std::size_t quadrant = 0; quadrant < childIds.size(); ++quadrant)
        {
            // Before allocation these are prospective assets, not child-owned hints.
            // Existing children keep their own hints even when the parent requests them.
            CacheIndex result;
            if (hasChildren)
            {
                auto& hint = m_residencyHints[node.children[quadrant]].heightmap;
                hint = m_heightmapManager.requestAsset(childIds[quadrant], hint);
                result = hint;
            }
            else
            {
                result = m_heightmapManager.requestAsset(childIds[quadrant]);
            }
            allChildrenResident =
                (result != kUnavailableCacheIndex) &&
                allChildrenResident;
        }

        if (allChildrenResident)
        {
            ensureChildren(nodeIndex, childIds);
            for (std::uint16_t childIndex : node.children)
            {
                updateNode(childIndex, activeCamera);
            }
        }
        return;
    }

    if (decision == LodDecision::Collapse && hasChildren)
    {
        auto& hint = m_residencyHints[nodeIndex].heightmap;
        hint = m_heightmapManager.requestAsset(node.nodeId, hint);
        if (hint != kUnavailableCacheIndex)
        {
            for (std::uint16_t& childIndex : node.children)
            {
                if (childIndex != QuadtreeNode::NullNodeIndex)
                {
                    freeSubtree(childIndex);
                    childIndex = QuadtreeNode::NullNodeIndex;
                }
            }
            node.currentStateStartFrame = m_currentFrameIndex;
            ++treeData.collapseCountThisFrame;
            recordTerrainLeafCount(node);
        }
        else
        {
            for (std::uint16_t childIndex : node.children)
            {
                updateNode(childIndex, activeCamera);
            }
        }
        return;
    }

    if (hasChildren)
    {
        for (std::uint16_t childIndex : node.children)
        {
            updateNode(childIndex, activeCamera);
        }
    }
}

WorldGridQuadtree::LodDecision WorldGridQuadtree::evaluateLodPolicy(
    std::uint16_t nodeIndex,
    const CameraManager::Camera& activeCamera) const
{
    const QuadtreeNode& node = m_nodes[nodeIndex];
    const double size = worldGridQuadtreeLeafSize(node.nodeId);
    if (size <= AppConfig::Quadtree::kMinimumQuadSize)
    {
        return nodeHasChildren(node) ? LodDecision::Collapse : LodDecision::Stay;
    }

    const glm::dvec3 cameraWorld = activeCamera.position.worldPosition();
    const double subdivisionDistance = size * AppConfig::Quadtree::kSubdivisionDistanceFactor;
    HeightmapExtents extents{};
    if (m_heightmapManager.buildExtents(node.nodeId, residentHeightmap(nodeIndex), extents))
    {
        double minHeight = static_cast<double>(extents.minHeight);
        double maxHeight = static_cast<double>(extents.maxHeight);
        if (m_waterVisibilityBoundsEnabled)
        {
            minHeight = std::min(minHeight, static_cast<double>(m_waterVisibilityMinHeight));
            maxHeight = std::max(maxHeight, static_cast<double>(m_waterVisibilityMaxHeight));
        }
        const auto [minCorner, maxCorner] = worldGridQuadtreeLeafBounds(node.nodeId, minHeight, maxHeight);
        const glm::dvec3 minWorld = minCorner.worldPosition();
        const glm::dvec3 maxWorld = maxCorner.worldPosition();
        const bool shouldSubdivide =
            distanceSquaredToBounds(
                cameraWorld.x,
                cameraWorld.y,
                cameraWorld.z,
                minWorld.x,
                minWorld.y,
                minWorld.z,
                maxWorld.x,
                maxWorld.y,
                maxWorld.z) <= subdivisionDistance * subdivisionDistance;
        return shouldSubdivide ? LodDecision::Subdivide : LodDecision::Collapse;
    }

    const auto [minCorner, maxCorner] = worldGridQuadtreeLeafBounds(node.nodeId);
    const glm::dvec3 nodeCenter = nodeCenterAtZeroHeight(minCorner, maxCorner);
    const glm::dvec3 toCenter = cameraWorld - nodeCenter;
    return glm::dot(toCenter, toCenter) <= subdivisionDistance * subdivisionDistance
        ? LodDecision::Subdivide
        : LodDecision::Collapse;
}

bool WorldGridQuadtree::nodeOccupied(std::uint16_t nodeIndex) const
{
    if (nodeIndex == QuadtreeNode::NullNodeIndex || nodeIndex >= m_nodes.size())
    {
        return false;
    }

    if (m_nodes[nodeIndex].parentIndex != QuadtreeNode::NullNodeIndex)
    {
        return true;
    }

    return std::find(m_baseNodes.begin(), m_baseNodes.end(), nodeIndex) != m_baseNodes.end();
}

bool WorldGridQuadtree::nodeHasChildren(const QuadtreeNode& node)
{
    return quadtreeNodeHasChildren(node);
}

std::uint8_t WorldGridQuadtree::quadrantInParent(std::uint16_t nodeIndex) const
{
    const QuadtreeNode& node = m_nodes[nodeIndex];
    if (node.parentIndex == QuadtreeNode::NullNodeIndex)
    {
        return 0u;
    }

    const QuadtreeNode& parent = m_nodes[node.parentIndex];
    for (std::uint8_t quadrant = 0; quadrant < parent.children.size(); ++quadrant)
    {
        if (parent.children[quadrant] == nodeIndex)
        {
            return quadrant;
        }
    }
    return 0u;
}

std::array<WorldGridQuadtreeLeafId, 4> WorldGridQuadtree::childIdsForNode(const QuadtreeNode& node)
{
    std::array<WorldGridQuadtreeLeafId, 4> childIds{};
    for (std::uint32_t quadrant = 0; quadrant < kQuadrantCount; ++quadrant)
    {
        childIds[quadrant] = {
            .gridX = node.nodeId.gridX,
            .gridY = node.nodeId.gridY,
            .subdivisionPath = WorldGridQuadtreeLeafId::appendChild(node.nodeId.subdivisionPath, quadrant),
        };
    }
    return childIds;
}

bool WorldGridQuadtree::nodeIsVisible(
    const QuadtreeNode& node,
    const WorldGridVisibilityFrustum& frustum,
    HeightmapExtents* extents) const
{
    double minHeight = extents != nullptr ? static_cast<double>(extents->minHeight) : -kVisibilityBoundsHalfHeight;
    double maxHeight = extents != nullptr ? static_cast<double>(extents->maxHeight) : kVisibilityBoundsHalfHeight;
    if (m_waterVisibilityBoundsEnabled)
    {
        minHeight = std::min(minHeight, static_cast<double>(m_waterVisibilityMinHeight));
        maxHeight = std::max(maxHeight, static_cast<double>(m_waterVisibilityMaxHeight));
    }

    const auto [minCorner, maxCorner] = worldGridQuadtreeLeafBounds(node.nodeId, minHeight, maxHeight);
    return shouldDrawNodeFrustum(minCorner, maxCorner, frustum);
}

bool WorldGridQuadtree::nodeIntersectsCanopyRange(const QuadtreeNode& node) const
{
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

bool WorldGridQuadtree::nodeIsInNearbyFoliageTopology(const QuadtreeNode& node) const
{
    if (std::abs(worldGridQuadtreeLeafSize(node.nodeId) - static_cast<double>(FoliageConfig::kPageSizeMeters)) > 0.001)
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

bool WorldGridQuadtree::nodeIntersectsNearbyFoliageRange(const QuadtreeNode& node, const HeightmapExtents& extents) const
{
    const auto [minCorner, maxCorner] = worldGridQuadtreeLeafBounds(
        node.nodeId,
        static_cast<double>(extents.minHeight),
        static_cast<double>(extents.maxHeight) + kNearbyFoliageTreeHeightPaddingMeters);
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

bool WorldGridQuadtree::nodeUsesCanonicalFoliagePages(const QuadtreeNode& node) const
{
    const double nodeSize = worldGridQuadtreeLeafSize(node.nodeId);
    return
        nodeSize >= static_cast<double>(FoliageConfig::kPageSizeMeters) &&
        nodeSize <= static_cast<double>(FoliageConfig::kPageSizeMeters * 4u);
}

bool WorldGridQuadtree::nodeCanUseCanopy(const QuadtreeNode& node) const
{
    return
        nodeIsCanopyLod(node) &&
        nodeIntersectsCanopyRange(node);
}

bool WorldGridQuadtree::nodeCanUseCanopyFallback(const QuadtreeNode& node) const
{
    const double nodeSize = worldGridQuadtreeLeafSize(node.nodeId);
    return
        std::abs(nodeSize - static_cast<double>(FoliageConfig::kPageSizeMeters * 4u)) <= 0.001 &&
        nodeIntersectsCanopyRange(node);
}

bool WorldGridQuadtree::nodeIsCanopyLod(const QuadtreeNode& node) const
{
    return std::abs(
        worldGridQuadtreeLeafSize(node.nodeId) -
        static_cast<double>(FoliageConfig::kCanopyNodeSizeMeters)) <= 0.001;
}

std::uint8_t WorldGridQuadtree::nodeStateFadeAgeFrames(const QuadtreeNode& node) const
{
    const std::uint64_t ageFrames = m_currentFrameIndex >= node.currentStateStartFrame
        ? m_currentFrameIndex - node.currentStateStartFrame
        : 0;
    return static_cast<std::uint8_t>(std::min<std::uint64_t>(
        ageFrames,
        FoliageConfig::kCanopyFadeInFrameCount));
}

std::uint8_t WorldGridQuadtree::canopyEdgeFadeStrength(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const
{
    const std::uint16_t neighborIndex = findNeighborSubtreeRoot(nodeIndex, edgeIndex);
    if (neighborIndex == QuadtreeNode::NullNodeIndex)
    {
        return 255u;
    }

    const QuadtreeNode& node = m_nodes[nodeIndex];
    const QuadtreeNode& neighbor = m_nodes[neighborIndex];
    const double nodeSize = worldGridQuadtreeLeafSize(node.nodeId);
    const double neighborSize = worldGridQuadtreeLeafSize(neighbor.nodeId);

    if (nodeHasChildren(neighbor))
    {
        return 0u;
    }

    if (neighborSize > nodeSize + kEdgeCoverageEpsilon)
    {
        return 255u;
    }

    if (std::abs(neighborSize - static_cast<double>(FoliageConfig::kCanopyNodeSizeMeters)) > kEdgeCoverageEpsilon)
    {
        return 0u;
    }

    const std::uint32_t fadeFrameCount = std::max<std::uint32_t>(FoliageConfig::kCanopyFadeInFrameCount, 1u);
    const std::uint32_t neighborAge = nodeStateFadeAgeFrames(neighbor);
    if (neighborAge >= fadeFrameCount)
    {
        return 0u;
    }

    return static_cast<std::uint8_t>(((fadeFrameCount - neighborAge) * 255u) / fadeFrameCount);
}

void WorldGridQuadtree::recordTerrainLeafCount(const QuadtreeNode& node)
{
    const std::uint8_t scalePow = worldGridQuadtreeLeafScalePow(node.nodeId);
    if (scalePow < treeData.terrainLeafCountByScalePow.size())
    {
        ++treeData.terrainLeafCountByScalePow[scalePow];
    }
}

std::uint16_t WorldGridQuadtree::findNeighborSubtreeRoot(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const
{
    if (!nodeOccupied(nodeIndex))
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

    const std::uint8_t quadrant = quadrantInParent(nodeIndex);
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
    if (!nodeHasChildren(parentNeighbor))
    {
        return parentNeighborIndex;
    }

    const std::uint8_t childQuadrant = descendantQuadrantForNeighbor(quadrant, edgeIndex);
    return childQuadrant < kQuadrantCount ? parentNeighbor.children[childQuadrant] : QuadtreeNode::NullNodeIndex;
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
        [this](std::uint16_t index)
        {
            return residentHeightmap(index) != kUnavailableCacheIndex;
        });
}

bool WorldGridQuadtree::subtreeEdgeCoveredByWater(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const
{
    return subtreeEdgeCoveredBy(
        m_nodes,
        nodeIndex,
        edgeIndex,
        [](std::uint16_t) { return true; });
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

std::optional<WorldGridQuadtree::CoarseTerrainNeighbor> WorldGridQuadtree::drawableCoarserNeighbor(
    std::uint16_t nodeIndex, std::uint8_t edgeIndex) const
{
    const std::uint16_t neighborRootIndex = findNeighborSubtreeRoot(nodeIndex, edgeIndex);
    if (neighborRootIndex == QuadtreeNode::NullNodeIndex)
    {
        return std::nullopt;
    }

    const QuadtreeNode& node = m_nodes[nodeIndex];
    const QuadtreeNode& neighborRoot = m_nodes[neighborRootIndex];
    const double nodeSize = worldGridQuadtreeLeafSize(node.nodeId);
    const double neighborSize = worldGridQuadtreeLeafSize(neighborRoot.nodeId);
    if (std::abs(neighborSize - (nodeSize * 2.0)) > kEdgeCoverageEpsilon)
        return std::nullopt;
    const CacheIndex sliceIndex = residentHeightmap(neighborRootIndex);
    if (sliceIndex == kUnavailableCacheIndex)
        return std::nullopt;
    const auto [nodeMin, nodeMax] = worldGridQuadtreeLeafBounds(node.nodeId);
    const auto [neighborMin, neighborMax] = worldGridQuadtreeLeafBounds(neighborRoot.nodeId);
    (void)nodeMax;
    (void)neighborMax;
    const glm::dvec3 offset = nodeMin.localCoordinatesInCellOf(neighborMin);
    const double parallelOffset = edgeIndex == kEdgeWest || edgeIndex == kEdgeEast ? offset.z : offset.x;
    return CoarseTerrainNeighbor{neighborRootIndex, sliceIndex,
                                 static_cast<std::uint8_t>(parallelOffset >= neighborSize * 0.5 ? 1u : 0u)};
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
    return std::abs(neighborSize - (nodeSize * 2.0)) <= kEdgeCoverageEpsilon;
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
