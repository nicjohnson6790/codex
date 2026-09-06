#pragma once

#include "WorldGridQuadtreeTypes.hpp"
#include <algorithm>

// Find the actual leaf across a corner, stopping once it cannot be coarser.
// A cached parent with children must never become a corner owner.
template<class FindRoot, class GetNode>
std::uint16_t terrainCoarseDiagonalNeighbor(
    const WorldGridQuadtreeLeafId& leaf, std::uint8_t corner,
    std::uint16_t unavailable, FindRoot findRoot, GetNode getNode)
{
    const double size = worldGridQuadtreeLeafSize(leaf);
    const auto [minimum, maximum] = worldGridQuadtreeLeafBounds(leaf);
    const bool east = corner == 1 || corner == 2;
    const bool north = corner == 2 || corner == 3;
    // Sample inside the diagonally adjacent fine-sized cell, avoiding boundary
    // ambiguity and retaining normalized grid coordinates across base roots.
    const Position probe = minimum.translated({east ? size * 1.25 : -size * 0.25,
                                               0.0, north ? size * 1.25 : -size * 0.25});
    auto index = findRoot(probe.gridX(), probe.gridY());
    while (index != unavailable)
    {
        const auto& node = getNode(index);
        double x, z, candidateSize;
        worldGridQuadtreeLeafExtents(node.nodeId, x, z, candidateSize);
        if (candidateSize <= size)
            return unavailable;
        if (std::none_of(node.children.begin(), node.children.end(),
                         [unavailable](std::uint16_t child) { return child != unavailable; }))
        {
            // Bridge corner normals currently support the same 2:1 pitch as edges.
            return candidateSize == size * 2.0 ? index : unavailable;
        }
        const bool childEast = probe.localPosition().x >= x + candidateSize * 0.5;
        const bool childNorth = probe.localPosition().z >= z + candidateSize * 0.5;
        index = node.children[(childNorth ? 0 : 2) + (childEast ? 0 : 1)];
    }
    return unavailable;
}
