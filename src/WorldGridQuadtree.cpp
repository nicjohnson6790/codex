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
    node.children.fill(QuadtreeNode::NullNodeIndex);
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

bool nodeHasFlag(const QuadtreeNode& node, std::uint8_t mask)
{
    return (node.flags & mask) != 0;
}

bool subtreeCanRenderWithoutParentFallback(
    const std::array<QuadtreeNode, WorldGridQuadtree::kNodeCapacity>& nodes,
    const QuadtreeNode& node)
{
    if (!nodeHasFlag(node, QuadtreeNode::IsUsedMask))
    {
        return true;
    }

    if (nodeHasFlag(node, QuadtreeNode::IsLeafMask))
    {
        return !nodeHasFlag(node, QuadtreeNode::IsUploadingMask);
    }

    if (nodeHasFlag(node, QuadtreeNode::IsSubdividingMask))
    {
        return !nodeHasFlag(node, QuadtreeNode::IsUploadingMask);
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
    if (nodeHasFlag(node, QuadtreeNode::IsLeafMask))
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
        if (!nodeHasFlag(child, QuadtreeNode::IsUsedMask) ||
            !nodeHasFlag(child, QuadtreeNode::IsLeafMask))
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

    for (const QuadtreeNode& node : m_nodes)
    {
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
            if (edgeHasDrawableNeighborCoverage(node, edgeIndex))
            {
                renderEngines.quadtreeMeshRenderer->addBridge(node.nodeId, sliceIndex, edgeIndex);
                continue;
            }

            if (edgeHasDrawableCoarserNeighbor(node, edgeIndex))
            {
                renderEngines.quadtreeMeshRenderer->addCoarseBridge(node.nodeId, sliceIndex, edgeIndex);
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

    for (const QuadtreeNode& node : m_nodes)
    {
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
            if (!edgeHasWaterNeighborCoverage(node, edgeIndex))
            {
                continue;
            }

            waterManager.requestBridge(
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
        child.children.fill(QuadtreeNode::NullNodeIndex);
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

bool WorldGridQuadtree::edgeHasDrawableNeighborCoverage(const QuadtreeNode& node, std::uint8_t edgeIndex) const
{
    const auto [nodeMinCorner, nodeMaxCorner] = worldGridQuadtreeLeafBounds(node.nodeId);
    const glm::dvec3 nodeMin = nodeMinCorner.worldPosition();
    const glm::dvec3 nodeMax = nodeMaxCorner.worldPosition();
    const double nodeSize = nodeMax.x - nodeMin.x;

    std::vector<std::pair<double, double>> coverageIntervals;
    coverageIntervals.reserve(8);

    for (const QuadtreeNode& candidate : m_nodes)
    {
        if (&candidate == &node || !nodeHasResidentTerrainSurface(candidate))
        {
            continue;
        }

        const auto [candidateMinCorner, candidateMaxCorner] = worldGridQuadtreeLeafBounds(candidate.nodeId);
        const glm::dvec3 candidateMin = candidateMinCorner.worldPosition();
        const glm::dvec3 candidateMax = candidateMaxCorner.worldPosition();
        const double candidateSize = candidateMax.x - candidateMin.x;
        if (candidateSize > nodeSize + kEdgeCoverageEpsilon)
        {
            continue;
        }

        double intervalStart = 0.0;
        double intervalEnd = 0.0;
        bool touchesEdge = false;

        switch (edgeIndex)
        {
        case 0:
            touchesEdge = std::abs(candidateMax.x - nodeMin.x) <= kEdgeCoverageEpsilon;
            intervalStart = std::max(nodeMin.z, candidateMin.z);
            intervalEnd = std::min(nodeMax.z, candidateMax.z);
            break;
        case 1:
            touchesEdge = std::abs(candidateMax.z - nodeMin.z) <= kEdgeCoverageEpsilon;
            intervalStart = std::max(nodeMin.x, candidateMin.x);
            intervalEnd = std::min(nodeMax.x, candidateMax.x);
            break;
        case 2:
            touchesEdge = std::abs(candidateMin.x - nodeMax.x) <= kEdgeCoverageEpsilon;
            intervalStart = std::max(nodeMin.z, candidateMin.z);
            intervalEnd = std::min(nodeMax.z, candidateMax.z);
            break;
        case 3:
            touchesEdge = std::abs(candidateMin.z - nodeMax.z) <= kEdgeCoverageEpsilon;
            intervalStart = std::max(nodeMin.x, candidateMin.x);
            intervalEnd = std::min(nodeMax.x, candidateMax.x);
            break;
        default:
            return false;
        }

        if (!touchesEdge || intervalEnd <= intervalStart + kEdgeCoverageEpsilon)
        {
            continue;
        }

        coverageIntervals.emplace_back(intervalStart, intervalEnd);
    }

    if (coverageIntervals.empty())
    {
        return false;
    }

    std::sort(coverageIntervals.begin(), coverageIntervals.end());
    const double targetStart = (edgeIndex == 0 || edgeIndex == 2) ? nodeMin.z : nodeMin.x;
    const double targetEnd = (edgeIndex == 0 || edgeIndex == 2) ? nodeMax.z : nodeMax.x;
    if (coverageIntervals.front().first > targetStart + kEdgeCoverageEpsilon)
    {
        return false;
    }

    double coveredEnd = coverageIntervals.front().second;
    if (coveredEnd >= targetEnd - kEdgeCoverageEpsilon)
    {
        return true;
    }

    for (std::size_t intervalIndex = 1; intervalIndex < coverageIntervals.size(); ++intervalIndex)
    {
        const auto& [intervalStart, intervalEnd] = coverageIntervals[intervalIndex];
        if (intervalStart > coveredEnd + kEdgeCoverageEpsilon)
        {
            return false;
        }

        coveredEnd = std::max(coveredEnd, intervalEnd);
        if (coveredEnd >= targetEnd - kEdgeCoverageEpsilon)
        {
            return true;
        }
    }

    return false;
}

bool WorldGridQuadtree::edgeHasDrawableCoarserNeighbor(const QuadtreeNode& node, std::uint8_t edgeIndex) const
{
    const auto [nodeMinCorner, nodeMaxCorner] = worldGridQuadtreeLeafBounds(node.nodeId);
    const glm::dvec3 nodeMin = nodeMinCorner.worldPosition();
    const glm::dvec3 nodeMax = nodeMaxCorner.worldPosition();
    const double nodeSize = nodeMax.x - nodeMin.x;
    const double expectedNeighborSize = nodeSize * 2.0;

    for (const QuadtreeNode& candidate : m_nodes)
    {
        if (&candidate == &node || !nodeHasResidentTerrainSurface(candidate))
        {
            continue;
        }

        const auto [candidateMinCorner, candidateMaxCorner] = worldGridQuadtreeLeafBounds(candidate.nodeId);
        const glm::dvec3 candidateMin = candidateMinCorner.worldPosition();
        const glm::dvec3 candidateMax = candidateMaxCorner.worldPosition();
        const double candidateSize = candidateMax.x - candidateMin.x;
        if (std::abs(candidateSize - expectedNeighborSize) > kEdgeCoverageEpsilon)
        {
            continue;
        }

        switch (edgeIndex)
        {
        case 0:
            if (std::abs(candidateMax.x - nodeMin.x) <= kEdgeCoverageEpsilon &&
                candidateMin.z <= nodeMin.z + kEdgeCoverageEpsilon &&
                candidateMax.z >= nodeMax.z - kEdgeCoverageEpsilon)
            {
                return true;
            }
            break;
        case 1:
            if (std::abs(candidateMax.z - nodeMin.z) <= kEdgeCoverageEpsilon &&
                candidateMin.x <= nodeMin.x + kEdgeCoverageEpsilon &&
                candidateMax.x >= nodeMax.x - kEdgeCoverageEpsilon)
            {
                return true;
            }
            break;
        case 2:
            if (std::abs(candidateMin.x - nodeMax.x) <= kEdgeCoverageEpsilon &&
                candidateMin.z <= nodeMin.z + kEdgeCoverageEpsilon &&
                candidateMax.z >= nodeMax.z - kEdgeCoverageEpsilon)
            {
                return true;
            }
            break;
        case 3:
            if (std::abs(candidateMin.z - nodeMax.z) <= kEdgeCoverageEpsilon &&
                candidateMin.x <= nodeMin.x + kEdgeCoverageEpsilon &&
                candidateMax.x >= nodeMax.x - kEdgeCoverageEpsilon)
            {
                return true;
            }
            break;
        default:
            return false;
        }
    }

    return false;
}

bool WorldGridQuadtree::edgeHasWaterNeighborCoverage(const QuadtreeNode& node, std::uint8_t edgeIndex) const
{
    const auto [nodeMinCorner, nodeMaxCorner] = worldGridQuadtreeLeafBounds(node.nodeId);
    const glm::dvec3 nodeMin = nodeMinCorner.worldPosition();
    const glm::dvec3 nodeMax = nodeMaxCorner.worldPosition();
    const double nodeSize = nodeMax.x - nodeMin.x;

    std::vector<std::pair<double, double>> coverageIntervals;
    coverageIntervals.reserve(8);

    for (const QuadtreeNode& candidate : m_nodes)
    {
        if (&candidate == &node || !nodeHasWaterSurface(candidate))
        {
            continue;
        }

        const auto [candidateMinCorner, candidateMaxCorner] = worldGridQuadtreeLeafBounds(candidate.nodeId);
        const glm::dvec3 candidateMin = candidateMinCorner.worldPosition();
        const glm::dvec3 candidateMax = candidateMaxCorner.worldPosition();
        const double candidateSize = candidateMax.x - candidateMin.x;
        if (candidateSize > nodeSize + kEdgeCoverageEpsilon)
        {
            continue;
        }

        double intervalStart = 0.0;
        double intervalEnd = 0.0;
        bool touchesEdge = false;

        switch (edgeIndex)
        {
        case 0:
            touchesEdge = std::abs(candidateMax.x - nodeMin.x) <= kEdgeCoverageEpsilon;
            intervalStart = std::max(nodeMin.z, candidateMin.z);
            intervalEnd = std::min(nodeMax.z, candidateMax.z);
            break;
        case 1:
            touchesEdge = std::abs(candidateMax.z - nodeMin.z) <= kEdgeCoverageEpsilon;
            intervalStart = std::max(nodeMin.x, candidateMin.x);
            intervalEnd = std::min(nodeMax.x, candidateMax.x);
            break;
        case 2:
            touchesEdge = std::abs(candidateMin.x - nodeMax.x) <= kEdgeCoverageEpsilon;
            intervalStart = std::max(nodeMin.z, candidateMin.z);
            intervalEnd = std::min(nodeMax.z, candidateMax.z);
            break;
        case 3:
            touchesEdge = std::abs(candidateMin.z - nodeMax.z) <= kEdgeCoverageEpsilon;
            intervalStart = std::max(nodeMin.x, candidateMin.x);
            intervalEnd = std::min(nodeMax.x, candidateMax.x);
            break;
        default:
            return false;
        }

        if (!touchesEdge || intervalEnd <= intervalStart + kEdgeCoverageEpsilon)
        {
            continue;
        }

        coverageIntervals.emplace_back(intervalStart, intervalEnd);
    }

    if (coverageIntervals.empty())
    {
        return false;
    }

    std::sort(coverageIntervals.begin(), coverageIntervals.end());
    const double targetStart = (edgeIndex == 0 || edgeIndex == 2) ? nodeMin.z : nodeMin.x;
    const double targetEnd = (edgeIndex == 0 || edgeIndex == 2) ? nodeMax.z : nodeMax.x;
    if (coverageIntervals.front().first > targetStart + kEdgeCoverageEpsilon)
    {
        return false;
    }

    double coveredEnd = coverageIntervals.front().second;
    if (coveredEnd >= targetEnd - kEdgeCoverageEpsilon)
    {
        return true;
    }

    for (std::size_t intervalIndex = 1; intervalIndex < coverageIntervals.size(); ++intervalIndex)
    {
        const auto& [intervalStart, intervalEnd] = coverageIntervals[intervalIndex];
        if (intervalStart > coveredEnd + kEdgeCoverageEpsilon)
        {
            return false;
        }

        coveredEnd = std::max(coveredEnd, intervalEnd);
        if (coveredEnd >= targetEnd - kEdgeCoverageEpsilon)
        {
            return true;
        }
    }

    return false;
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
