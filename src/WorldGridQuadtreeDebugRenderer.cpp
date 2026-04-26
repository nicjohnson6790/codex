#include "WorldGridQuadtreeDebugRenderer.hpp"

#include "AppConfig.hpp"
#include "LineRenderer.hpp"
#include "PerformanceCapture.hpp"

#include <algorithm>

void WorldGridQuadtreeDebugRenderer::appendNodeBorder(
    RenderEngines& renderEngines,
    const WorldGridQuadtreeLeafId& leafId,
    std::uint32_t maxDepth,
    bool hasExtents,
    float minHeight,
    float maxHeight) const
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
    if (!hasExtents)
    {
        const Position p00(leafId.gridX, leafId.gridY, { localMinX, 0.0, localMinZ });
        const Position p10(leafId.gridX, leafId.gridY, { localMinX + size, 0.0, localMinZ });
        const Position p11(leafId.gridX, leafId.gridY, { localMinX + size, 0.0, localMinZ + size });
        const Position p01(leafId.gridX, leafId.gridY, { localMinX, 0.0, localMinZ + size });

        renderEngines.lineRenderer.addLine(p00, p10, color);
        renderEngines.lineRenderer.addLine(p10, p11, color);
        renderEngines.lineRenderer.addLine(p11, p01, color);
        renderEngines.lineRenderer.addLine(p01, p00, color);
        return;
    }

    const double minY = static_cast<double>(minHeight);
    const double maxY = static_cast<double>(maxHeight);

    const Position p000(leafId.gridX, leafId.gridY, { localMinX, minY, localMinZ });
    const Position p100(leafId.gridX, leafId.gridY, { localMinX + size, minY, localMinZ });
    const Position p110(leafId.gridX, leafId.gridY, { localMinX + size, minY, localMinZ + size });
    const Position p010(leafId.gridX, leafId.gridY, { localMinX, minY, localMinZ + size });

    const Position p001(leafId.gridX, leafId.gridY, { localMinX, maxY, localMinZ });
    const Position p101(leafId.gridX, leafId.gridY, { localMinX + size, maxY, localMinZ });
    const Position p111(leafId.gridX, leafId.gridY, { localMinX + size, maxY, localMinZ + size });
    const Position p011(leafId.gridX, leafId.gridY, { localMinX, maxY, localMinZ + size });

    renderEngines.lineRenderer.addLine(p000, p100, color);
    renderEngines.lineRenderer.addLine(p100, p110, color);
    renderEngines.lineRenderer.addLine(p110, p010, color);
    renderEngines.lineRenderer.addLine(p010, p000, color);

    renderEngines.lineRenderer.addLine(p001, p101, color);
    renderEngines.lineRenderer.addLine(p101, p111, color);
    renderEngines.lineRenderer.addLine(p111, p011, color);
    renderEngines.lineRenderer.addLine(p011, p001, color);

    renderEngines.lineRenderer.addLine(p000, p001, color);
    renderEngines.lineRenderer.addLine(p100, p101, color);
    renderEngines.lineRenderer.addLine(p110, p111, color);
    renderEngines.lineRenderer.addLine(p010, p011, color);
}
