#pragma once

#include "AppConfig.hpp"
#include "CameraManager.hpp"
#include "WorldGridQuadtreeDebugRenderer.hpp"
#include "WorldGridQuadtreeHeightmapManager.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

class RenderEngines;

struct QuadtreeNode
{
    WorldGridQuadtreeLeafId nodeId{};
    std::array<std::uint16_t, 4> children{
        std::uint16_t{0xFFFF},
        std::uint16_t{0xFFFF},
        std::uint16_t{0xFFFF},
        std::uint16_t{0xFFFF},
    };
    std::uint8_t flags = 0;

    static constexpr std::uint16_t NullNodeIndex = UINT16_MAX;
    static constexpr std::uint8_t IsLeafMask = 1;
    static constexpr std::uint8_t IsVisibleMask = 2;
    static constexpr std::uint8_t IsUsedMask = 4;
    // Parent remains the visible fallback while children finish becoming drawable.
    static constexpr std::uint8_t IsSubdividingMask = 8;
    // Existing children remain the visible fallback while the parent becomes drawable again.
    static constexpr std::uint8_t IsCollapsingMask = 16;
    // This node's own slice is not resident yet.
    static constexpr std::uint8_t IsUploadingMask = 32;
};

class WorldGridQuadtree
{
public:
    struct TreeData
    {
        std::uint16_t visibleNodeCount = 0;
        std::uint16_t subdivisionCountThisFrame = 0;
        std::uint16_t collapseCountThisFrame = 0;
        std::uint32_t maxDepth = 0;
    };

    static constexpr std::size_t kNodeCapacity = 512;
    static constexpr std::size_t kBaseNodeCount = 9;

    WorldGridQuadtree();

    WorldGridQuadtree(const WorldGridQuadtree&) = delete;
    WorldGridQuadtree& operator=(const WorldGridQuadtree&) = delete;

    void updateTree(const CameraManager::Camera& activeCamera);
    void emitMeshDraws(RenderEngines& renderEngines);
    void emitDebugDraws(RenderEngines& renderEngines) const;
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
    [[nodiscard]] static bool nodeHasFlag(const QuadtreeNode& node, std::uint8_t mask);
    static void setNodeFlag(QuadtreeNode& node, std::uint8_t mask, bool enabled);
    [[nodiscard]] static bool shouldSubdivide(const WorldGridQuadtreeLeafId& leafId, const Position& cameraPosition);
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
};
