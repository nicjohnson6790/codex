#include "WorldGridQuadtreeDebugRenderer.hpp"

#include "AppConfig.hpp"
#include "LineRenderer.hpp"
#include "PerformanceCapture.hpp"

#include <algorithm>

void WorldGridQuadtreeDebugRenderer::appendNodeBorder(
    RenderEngines& renderEngines,
    const WorldGridQuadtreeLeafId& leafId,
    std::uint32_t maxDepth) const
{
    HELLO_PROFILE_SCOPE("WorldGridQuadtreeDebugRenderer::AppendNodeBorder");

    const std::uint32_t depth = worldGridQuadtreeLeafDepth(leafId);
    double localMinX = 0.0;
    double localMinZ = 0.0;
    double size = 0.0;
    worldGridQuadtreeLeafExtents(leafId, localMinX, localMinZ, size);

    const float depthT = static_cast<float>(depth) / static_cast<float>(std::max<std::uint32_t>(maxDepth, 1));
    const glm::vec3 color(
        0.2f + (0.35f * depthT),
        0.45f + (0.45f * depthT),
        0.95f - (0.3f * depthT)
    );
    const double height = AppConfig::Quadtree::kDebugBaseHeightOffset +
        (AppConfig::Quadtree::kDebugDepthHeightOffset * static_cast<double>(depth));

    const Position p00(leafId.gridX, leafId.gridY, { localMinX, height, localMinZ });
    const Position p10(leafId.gridX, leafId.gridY, { localMinX + size, height, localMinZ });
    const Position p11(leafId.gridX, leafId.gridY, { localMinX + size, height, localMinZ + size });
    const Position p01(leafId.gridX, leafId.gridY, { localMinX, height, localMinZ + size });

    renderEngines.lineRenderer.addLine(p00, p10, color);
    renderEngines.lineRenderer.addLine(p10, p11, color);
    renderEngines.lineRenderer.addLine(p11, p01, color);
    renderEngines.lineRenderer.addLine(p01, p00, color);
}
