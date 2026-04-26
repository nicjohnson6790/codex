#pragma once

#include "AppConfig.hpp"
#include "Position.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>

struct WorldGridQuadtreeLeafId
{
    std::int64_t gridX = 0;
    std::int64_t gridY = 0;

    // base node is 0, it's 0 quadrant is 4 (100), it's 1 quadrant is 5 (101), it's 1 quadrant's 2 quadrant is 53 (110,101)
    std::uint64_t subdivisionPath = 0;

    [[nodiscard]] bool operator==(const WorldGridQuadtreeLeafId& other) const = default;

    [[nodiscard]] static constexpr std::uint64_t appendChild(std::uint64_t path, std::uint32_t quadrant)
    {
        std::uint32_t depth = 0;
        std::uint64_t cursor = path;
        while (cursor != 0)
        {
            ++depth;
            cursor >>= 3U;
        }

        const std::uint64_t childChunk = 0x4ULL | static_cast<std::uint64_t>(quadrant & 0x3U);
        return path | (childChunk << (depth * 3U));
    }
};

[[nodiscard]] inline std::uint32_t worldGridQuadtreeLeafDepth(const WorldGridQuadtreeLeafId& leafId)
{
    std::uint32_t depth = 0;
    std::uint64_t path = leafId.subdivisionPath;
    while (path != 0)
    {
        ++depth;
        path >>= 3U;
    }
    return depth;
}

inline void worldGridQuadtreeLeafExtents(const WorldGridQuadtreeLeafId& leafId, double& localMinX, double& localMinZ, double& size)
{
    std::uint64_t path = leafId.subdivisionPath;

    localMinX = 0.0;
    localMinZ = 0.0;
    size = static_cast<double>(Position::kCellSize);

    while (path != 0)
    {
        size *= 0.5;
        switch (static_cast<std::uint32_t>(path & 0x3U))
        {
        case 0:
            localMinX += size;
            localMinZ += size;
            break;
        case 1:
            localMinZ += size;
            break;
        case 2:
            localMinX += size;
            break;
        case 3:
            break;
        default:
            break;
        }

        path >>= 3U;
    }
}

[[nodiscard]] inline double worldGridQuadtreeLeafSize(const WorldGridQuadtreeLeafId& leafId)
{
    double localMinX = 0.0;
    double localMinZ = 0.0;
    double size = 0.0;
    worldGridQuadtreeLeafExtents(leafId, localMinX, localMinZ, size);
    return size;
}

[[nodiscard]] inline std::pair<Position, Position> worldGridQuadtreeLeafBounds(
    const WorldGridQuadtreeLeafId& leafId,
    double minY = 0.0,
    double maxY = 0.0)
{
    double localMinX = 0.0;
    double localMinZ = 0.0;
    double size = 0.0;
    worldGridQuadtreeLeafExtents(leafId, localMinX, localMinZ, size);

    return {
        Position(leafId.gridX, leafId.gridY, { localMinX, minY, localMinZ }),
        Position(leafId.gridX, leafId.gridY, { localMinX + size, maxY, localMinZ + size }),
    };
}

[[nodiscard]] inline std::uint8_t worldGridQuadtreeLeafScalePow(const WorldGridQuadtreeLeafId& leafId)
{
    double size = worldGridQuadtreeLeafSize(leafId);
    std::uint8_t scalePow = 0;
    double currentSize = AppConfig::Quadtree::kMinimumQuadSize;
    while (currentSize < size)
    {
        currentSize *= 2.0;
        ++scalePow;
    }

    return scalePow;
}
