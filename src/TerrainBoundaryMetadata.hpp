#pragma once

#include <cassert>
#include <array>
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

// Boundary slices are separate fields; the low bits encode corner selectors.
// Edge order W,S,E,N. Endpoint order follows the old perimeter winding.
inline constexpr std::array<std::array<std::uint8_t, 2>, 4> kBoundaryEdgeCorners{{{{0, 3}}, {{1, 0}}, {{2, 1}}, {{3, 2}}}};
// Bits 0-2/3-5 are corner selectors; bit 26 is the coarse half.
// Submission adds bit 31 for a 2:1 edge. All other bits are zero.
constexpr std::uint32_t terrainBoundaryMetadata(
    std::uint8_t half, std::uint8_t firstCorner, std::uint8_t secondCorner)
{
    return (firstCorner & 7u) | ((secondCorner & 7u) << 3u) |
           ((half & 1u) << 26u);
}
