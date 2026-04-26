#include "WorldGridQuadtree.hpp"

#include "PerformanceCapture.hpp"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>

namespace
{
bool aabbIsVisible(
    const Position& aabbCornerA,
    const Position& aabbCornerB,
    const CameraManager::Camera& activeCamera)
{
    const glm::dvec3 forward = glm::normalize(activeCamera.forward);
    glm::dvec3 right = glm::cross(forward, activeCamera.up);
    if (glm::length(right) <= 0.00001)
    {
        right = { 1.0, 0.0, 0.0 };
    }
    right = glm::normalize(right);
    const glm::dvec3 up = glm::normalize(glm::cross(right, forward));

    const glm::dvec3 minCorner = aabbCornerA.localCoordinatesInCellOf(activeCamera.position);
    const glm::dvec3 maxCorner = aabbCornerB.localCoordinatesInCellOf(activeCamera.position);

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

    constexpr double kAspectRatio = 16.0 / 9.0;
    const double tanHalfVerticalFov = std::tan(AppConfig::Camera::kVerticalFovRadians * 0.5);
    const double tanHalfHorizontalFov = tanHalfVerticalFov * kAspectRatio;

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
    if (!nodeHasFlag(node, QuadtreeNode::IsUsedMask) ||
        !nodeHasFlag(node, QuadtreeNode::IsVisibleMask))
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

void WorldGridQuadtree::updateTree(const CameraManager::Camera& activeCamera)
{
    HELLO_PROFILE_SCOPE("WorldGridQuadtree::UpdateTree");

    refreshBaseNodes(activeCamera.position);
    m_heightmapManager.ageMap();
    treeData = {};

    for (const std::uint16_t nodeIndex : m_baseNodes)
    {
        if (nodeIndex != QuadtreeNode::NullNodeIndex)
        {
            updateNode(nodeIndex, activeCamera);
        }
    }
}

void WorldGridQuadtree::emitMeshDraws(RenderEngines& renderEngines)
{
    if (renderEngines.quadtreeMeshRenderer == nullptr)
    {
        return;
    }

    m_heightmapManager.uploadFromQueue(*renderEngines.quadtreeMeshRenderer);

    for (const QuadtreeNode& node : m_nodes)
    {
        if (!nodeHasFlag(node, QuadtreeNode::IsUsedMask) ||
            !nodeHasFlag(node, QuadtreeNode::IsVisibleMask))
        {
            continue;
        }

        if (nodeHasFlag(node, QuadtreeNode::IsLeafMask))
        {
            if (!nodeHasFlag(node, QuadtreeNode::IsUploadingMask))
            {
                m_heightmapManager.requestLeaf(node.nodeId, *renderEngines.quadtreeMeshRenderer);
            }
            continue;
        }

        if (nodeHasFlag(node, QuadtreeNode::IsSubdividingMask) &&
            !nodeHasFlag(node, QuadtreeNode::IsUploadingMask))
        {
            // While subdividing, the parent stays visible until the child subtrees can cover its area.
            m_heightmapManager.requestLeaf(node.nodeId, *renderEngines.quadtreeMeshRenderer);
        }
    }
}

void WorldGridQuadtree::emitDebugDraws(RenderEngines& renderEngines) const
{
    for (const QuadtreeNode& node : m_nodes)
    {
        if (nodeHasFlag(node, QuadtreeNode::IsUsedMask) &&
            nodeHasFlag(node, QuadtreeNode::IsVisibleMask))
        {
            m_debugRenderer.appendNodeBorder(renderEngines, node.nodeId, treeData.maxDepth);
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
    if (m_hasBaseGrid &&
        m_baseGridX == cameraPosition.gridX() &&
        m_baseGridY == cameraPosition.gridY())
    {
        return;
    }

    reset();
    m_baseGridX = cameraPosition.gridX();
    m_baseGridY = cameraPosition.gridY();
    m_hasBaseGrid = true;

    std::size_t baseNodeArrayIndex = 0;
    for (int gridOffsetY = -AppConfig::Quadtree::kNeighborRadius; gridOffsetY <= AppConfig::Quadtree::kNeighborRadius; ++gridOffsetY)
    {
        for (int gridOffsetX = -AppConfig::Quadtree::kNeighborRadius; gridOffsetX <= AppConfig::Quadtree::kNeighborRadius; ++gridOffsetX)
        {
            const std::uint16_t nodeIndex = allocateNode();
            QuadtreeNode& node = m_nodes[nodeIndex];
            node.nodeId = {
                .gridX = cameraPosition.gridX() + static_cast<std::int64_t>(gridOffsetX),
                .gridY = cameraPosition.gridY() + static_cast<std::int64_t>(gridOffsetY),
                .subdivisionPath = 0,
            };
            node.children.fill(QuadtreeNode::NullNodeIndex);
            node.flags = QuadtreeNode::IsUsedMask | QuadtreeNode::IsLeafMask;
            m_baseNodes[baseNodeArrayIndex++] = nodeIndex;
        }
    }
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

void WorldGridQuadtree::updateNode(std::uint16_t nodeIndex, const CameraManager::Camera& activeCamera)
{
    QuadtreeNode& node = m_nodes[nodeIndex];
    if (!nodeHasFlag(node, QuadtreeNode::IsUsedMask))
    {
        return;
    }

    const double size = worldGridQuadtreeLeafSize(node.nodeId);
    const auto [minCorner, maxCorner] = worldGridQuadtreeLeafBounds(
        node.nodeId,
        static_cast<double>(AppConfig::Terrain::kBaseHeight - AppConfig::Terrain::kHeightAmplitude),
        static_cast<double>(AppConfig::Terrain::kBaseHeight + AppConfig::Terrain::kHeightAmplitude));

    const bool isVisible = aabbIsVisible(minCorner, maxCorner, activeCamera);
    setNodeFlag(node, QuadtreeNode::IsVisibleMask, isVisible);
    treeData.maxDepth = std::max(treeData.maxDepth, worldGridQuadtreeLeafDepth(node.nodeId));
    if (isVisible)
    {
        ++treeData.visibleNodeCount;
    }

    if (!isVisible)
    {
        if (!nodeHasFlag(node, QuadtreeNode::IsLeafMask))
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
        }

        setNodeFlag(node, QuadtreeNode::IsSubdividingMask, false);
        setNodeFlag(node, QuadtreeNode::IsCollapsingMask, false);
        setNodeFlag(node, QuadtreeNode::IsUploadingMask, false);
        return;
    }

    const bool wantsChildren =
        size > AppConfig::Quadtree::kMinimumQuadSize &&
        shouldSubdivide(node.nodeId, activeCamera.position);

    if (wantsChildren)
    {
        ensureChildren(nodeIndex);
        if (nodeHasFlag(node, QuadtreeNode::IsLeafMask))
        {
            const bool resident = m_heightmapManager.makeResident(node.nodeId);
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
    setNodeFlag(node, QuadtreeNode::IsUploadingMask, !resident);
    setNodeFlag(node, QuadtreeNode::IsSubdividingMask, false);
    // While collapsing, the children remain visible until the parent slice is resident again.
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

bool WorldGridQuadtree::shouldSubdivide(const WorldGridQuadtreeLeafId& leafId, const Position& cameraPosition)
{
    const double size = worldGridQuadtreeLeafSize(leafId);

    const double cellSize = static_cast<double>(Position::kCellSize);
    const auto [minCorner, maxCorner] = worldGridQuadtreeLeafBounds(
        leafId,
        static_cast<double>(AppConfig::Terrain::kBaseHeight - AppConfig::Terrain::kHeightAmplitude),
        static_cast<double>(AppConfig::Terrain::kBaseHeight + AppConfig::Terrain::kHeightAmplitude));
    const double minX = (static_cast<double>(minCorner.gridX()) * cellSize) + minCorner.localPosition().x;
    const double minZ = (static_cast<double>(minCorner.gridY()) * cellSize) + minCorner.localPosition().z;
    const double maxX = (static_cast<double>(maxCorner.gridX()) * cellSize) + maxCorner.localPosition().x;
    const double maxZ = (static_cast<double>(maxCorner.gridY()) * cellSize) + maxCorner.localPosition().z;
    const double minY = static_cast<double>(AppConfig::Terrain::kBaseHeight - AppConfig::Terrain::kHeightAmplitude);
    const double maxY = static_cast<double>(AppConfig::Terrain::kBaseHeight + AppConfig::Terrain::kHeightAmplitude);

    const glm::dvec3 cameraWorld = cameraPosition.worldPosition();
    const double subdivisionDistance = size * AppConfig::Quadtree::kSubdivisionDistanceFactor;

    return distanceSquaredToBounds(
        cameraWorld.x,
        cameraWorld.y,
        cameraWorld.z,
        minX,
        minY,
        minZ,
        maxX,
        maxY,
        maxZ) <= subdivisionDistance * subdivisionDistance;
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
