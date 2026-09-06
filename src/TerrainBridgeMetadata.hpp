#pragma once

#include <cassert>
#include <cstdint>

// Owner-relative perimeter positions, in row order with the center omitted:
// NW N NE     5 6 7
// W     E     3   4
// SW S SE     0 1 2
// Local corner order: SW, SE, NE, NW.
constexpr std::uint8_t terrainOwnCornerSelector(std::uint8_t corner)
{
    constexpr std::uint8_t selectors[]{0, 2, 7, 5};
    assert(corner < 4);
    return selectors[corner];
}

constexpr std::uint8_t terrainDiagonalCornerSelector(std::uint8_t corner)
{
    return terrainOwnCornerSelector((corner + 2u) & 3u);
}

// An edge neighbor exposes three positions: its two corners and edge midpoint.
// half selects the lower/upper half; corner selects that half's endpoint.
constexpr std::uint8_t terrainEdgeCornerSelector(std::uint8_t edge, std::uint8_t half,
                                                std::uint8_t corner)
{
    constexpr std::uint8_t selectors[4][3]{
        {2, 4, 7}, // west neighbor: SE, E, NE
        {5, 6, 7}, // south neighbor: NW, N, NE
        {0, 3, 5}, // east neighbor: SW, W, NW
        {0, 1, 2}, // north neighbor: SW, S, SE
    };
    assert(edge < 4 && half < 2 && corner < 4);
    const bool upperEndpoint = (edge == 0 || edge == 2)
        ? corner >= 2 : (corner == 1 || corner == 2);
    return selectors[edge][half + static_cast<std::uint8_t>(upperEndpoint)];
}

// Bridge slices live in heightmapIndices, so the low metadata bits are free.
// Keep scale/edge/half in their existing shader locations.
constexpr std::uint32_t terrainBridgeMetadata(std::uint8_t scale, std::uint8_t edge,
    std::uint8_t half, std::uint8_t firstCorner, std::uint8_t secondCorner)
{
    return (firstCorner & 7u) | ((secondCorner & 7u) << 3u) |
           (static_cast<std::uint32_t>(scale) << 16u) |
           ((edge & 3u) << 24u) | ((half & 1u) << 26u);
}
