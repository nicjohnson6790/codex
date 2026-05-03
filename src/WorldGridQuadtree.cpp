#include "WorldGridQuadtree.hpp"

#include "PerformanceCapture.hpp"
#include "QuadtreeMeshRenderer.hpp"
#include "WorldGridQuadtreeWaterManager.hpp"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>
#include <utility>
#include <vector>

namespace
{
constexpr double kVisibilityBoundsHalfHeight = 10000.0;
constexpr double kEdgeCoverageEpsilon = 1.0e-4;
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

    const glm::dvec3 corners[8]{
        { minCorner.x, minCorner.y, minCorner.z },
        { maxCorner.x, minCorner.y, minCorner.z },
        { minCorner.x, maxCorner.y, minCorner.z },
        { maxCorner.x, maxCorner.y, minCorner.z },
        { minCorner.x, minCorner.y, maxCorner.z },
        { maxCorner.x, minCorner.y, maxCorner.z },
        { minCorner.x, maxCorner.y, maxCorner.z },
        { maxCorner.x, maxCorner.y, maxCorner.z },
    };

    const double aspectRatio =
        static_cast<double>(std::max(viewportExtent.width, 1u)) /
        static_cast<double>(std::max(viewportExtent.height, 1u));
    const double tanHalfVerticalFov = std::tan(AppConfig::Camera::kVerticalFovRadians * 0.5);
    const double tanHalfHorizontalFov = tanHalfVerticalFov * aspectRatio;

    bool anyInFrontOfNearPlane = false;
    bool allLeftOfFrustum = true;
    bool allRightOfFrustum = true;
    bool allBelowFrustum = true;
    bool allAboveFrustum = true;

    for (const glm::dvec3& corner : corners)
    {
        const double localX = glm::dot(corner, right);
        const double localY = glm::dot(corner, up);
        const double localZ = glm::dot(corner, forward);

        if (localZ >= AppConfig::Camera::kNearPlane)
        {
            anyInFrontOfNearPlane = true;
        }

        const double horizontalLimit = localZ * tanHalfHorizontalFov;
        const double verticalLimit = localZ * tanHalfVerticalFov;

        if (localX >= -horizontalLimit)
        {
            allLeftOfFrustum = false;
        }
        if (localX <= horizontalLimit)
        {
            allRightOfFrustum = false;
        }
        if (localY >= -verticalLimit)
        {
            allBelowFrustum = false;
        }
        if (localY <= verticalLimit)
        {
            allAboveFrustum = false;
        }
    }

    return
        anyInFrontOfNearPlane &&
        !allLeftOfFrustum &&
        !allRightOfFrustum &&
        !allBelowFrustum &&
        !allAboveFrustum;
}

bool quadtreeNodeHasFlag(const QuadtreeNode& node, std::uint8_t mask)
{
    return (node.flags & mask) != 0;
}

bool subtreeCanRenderWithoutParentFallback(
    const std::array<QuadtreeNode, WorldGridQuadtree::kNodeCapacity>& nodes,
    const QuadtreeNode& node)
{
    if (!quadtreeNodeHasFlag(node, QuadtreeNode::IsUsedMask))
    {
        return true;
    }

    if (quadtreeNodeHasFlag(node, QuadtreeNode::IsLeafMask))
    {
        return !quadtreeNodeHasFlag(node, QuadtreeNode::IsUploadingMask);
    }

    if (quadtreeNodeHasFlag(node, QuadtreeNode::IsSubdividingMask))
    {
        return !quadtreeNodeHasFlag(node, QuadtreeNode::IsUploadingMask);
    }

    for (const std::uint16_t childIndex : node.children)
    {
        if (childIndex == QuadtreeNode::NullNodeIndex)
        {
            continue;
        }

        if (!subtreeCanRenderWithoutParentFallback(nodes, nodes[childIndex]))
        {
            return false;
        }
    }

    return true;
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
    m_heightmapManager.dispatchFromQueue(meshRenderer);
}

void WorldGridQuadtree::updateTree(const CameraManager::Camera& activeCamera, Extent2D viewportExtent)
{
    HELLO_PROFILE_SCOPE("WorldGridQuadtree::UpdateTree");

    m_viewportExtent = viewportExtent;
    {
        HELLO_PROFILE_SCOPE("WorldGridQuadtree::RefreshBaseNodes");
        refreshBaseNodes(activeCamera.position);
    }
    {
        HELLO_PROFILE_SCOPE("WorldGridQuadtree::AgeHeightmapResidency");
        m_heightmapManager.ageMap();
    }
    treeData = {};

    {
        HELLO_PROFILE_SCOPE("WorldGridQuadtree::UpdateBaseNodes");
        for (const std::uint16_t nodeIndex : m_baseNodes)
        {
            if (nodeIndex != QuadtreeNode::NullNodeIndex)
            {
                updateNode(nodeIndex, activeCamera);
            }
        }
    }
}

void WorldGridQuadtree::emitMeshDraws(RenderEngines& renderEngines)
{
    HELLO_PROFILE_SCOPE("WorldGridQuadtree::EmitMeshDraws");

    if (renderEngines.quadtreeMeshRenderer == nullptr)
    {
        return;
    }

    for (std::uint16_t nodeIndex = 0; nodeIndex < static_cast<std::uint16_t>(m_nodes.size()); ++nodeIndex)
    {
        const QuadtreeNode& node = m_nodes[nodeIndex];
        if (!nodeContributesTerrainDraw(node))
        {
            continue;
        }

        std::uint16_t sliceIndex = 0;
        if (!m_heightmapManager.getResidentSliceIndex(node.nodeId, sliceIndex))
        {
            continue;
        }

        m_heightmapManager.requestLeaf(node.nodeId, *renderEngines.quadtreeMeshRenderer);

        for (std::uint8_t edgeIndex = 0; edgeIndex < 4; ++edgeIndex)
        {
            // Every drawn terrain node needs one bridge per edge because the main patch
            // is trimmed inward by one sample. The only decision here is whether the edge
            // should use the regular 1:1 bridge or the 2:1 coarse bridge.
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

void WorldGridQuadtree::emitWaterDraws(WorldGridQuadtreeWaterManager& waterManager) const
{
    HELLO_PROFILE_SCOPE("WorldGridQuadtree::EmitWaterDraws");

    for (std::uint16_t nodeIndex = 0; nodeIndex < static_cast<std::uint16_t>(m_nodes.size()); ++nodeIndex)
    {
        const QuadtreeNode& node = m_nodes[nodeIndex];
        if (!nodeContributesWaterDraw(node))
        {
            continue;
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
            continue;
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

std::uint16_t WorldGridQuadtree::findNeighborSubtreeRoot(std::uint16_t nodeIndex, std::uint8_t edgeIndex) const
{
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

void WorldGridQuadtree::updateNode(std::uint16_t nodeIndex, const CameraManager::Camera& activeCamera)
{
    QuadtreeNode& node = m_nodes[nodeIndex];
    if (!nodeHasFlag(node, QuadtreeNode::IsUsedMask))
    {
        return;
    }

    const double size = worldGridQuadtreeLeafSize(node.nodeId);
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
            setNodeFlag(node, QuadtreeNode::IsSubdividingMask, false);
            setNodeFlag(node, QuadtreeNode::IsCollapsingMask, false);
            return;
        }

        bool allChildrenReady = true;
        for (const std::uint16_t childIndex : node.children)
        {
            if (childIndex == QuadtreeNode::NullNodeIndex)
            {
                allChildrenReady = false;
                continue;
            }

            updateNode(childIndex, activeCamera);
            allChildrenReady = allChildrenReady &&
                subtreeCanRenderWithoutParentFallback(m_nodes, m_nodes[childIndex]);
        }

        if (!nodeIsParentOfLeaves(m_nodes, node))
        {
            setNodeFlag(node, QuadtreeNode::IsUploadingMask, false);
            setNodeFlag(node, QuadtreeNode::IsSubdividingMask, false);
            setNodeFlag(node, QuadtreeNode::IsCollapsingMask, false);
            return;
        }

        const bool wasSubdividing = nodeHasFlag(node, QuadtreeNode::IsSubdividingMask);
        const bool resident = m_heightmapManager.makeResident(node.nodeId);
        if (resident && !nodeHasFlag(node, QuadtreeNode::HasExtentsMask))
        {
            applyKnownExtentsToNode(node);
        }
        setNodeFlag(node, QuadtreeNode::IsUploadingMask, !resident);

        const bool isSubdividing = !allChildrenReady;
        setNodeFlag(node, QuadtreeNode::IsSubdividingMask, isSubdividing);
        setNodeFlag(node, QuadtreeNode::IsCollapsingMask, false);
        if (isSubdividing && !wasSubdividing)
        {
            ++treeData.subdivisionCountThisFrame;
        }
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
        setNodeFlag(node, QuadtreeNode::IsSubdividingMask, false);
        setNodeFlag(node, QuadtreeNode::IsCollapsingMask, false);
        return;
    }

    for (const std::uint16_t childIndex : node.children)
    {
        if (childIndex != QuadtreeNode::NullNodeIndex)
        {
            updateNode(childIndex, activeCamera);
        }
    }

    if (!nodeIsParentOfLeaves(m_nodes, node))
    {
        setNodeFlag(node, QuadtreeNode::IsUploadingMask, false);
        setNodeFlag(node, QuadtreeNode::IsSubdividingMask, false);
        setNodeFlag(node, QuadtreeNode::IsCollapsingMask, false);
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
    // While collapsing, the children remain drawable until the parent slice is resident again.
    setNodeFlag(node, QuadtreeNode::IsCollapsingMask, !resident);

    if (resident)
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
        return;
    }

    if (!wasCollapsing)
    {
        ++treeData.collapseCountThisFrame;
    }
}

bool WorldGridQuadtree::nodeHasFlag(const QuadtreeNode& node, std::uint8_t mask)
{
    return (node.flags & mask) != 0;
}

void WorldGridQuadtree::setNodeFlag(QuadtreeNode& node, std::uint8_t mask, bool enabled)
{
    if (enabled)
    {
        node.flags |= mask;
    }
    else
    {
        node.flags &= static_cast<std::uint8_t>(~mask);
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
