#include "TerrainCornerNeighbor.hpp"
#include "TerrainBridgeMetadata.hpp"
#include "ParentMeshDescriptors.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

constexpr std::uint16_t none = 65535;
struct Node
{
    WorldGridQuadtreeLeafId nodeId{};
    std::array<std::uint16_t, 4> children{none, none, none, none};
};

int main()
{
    int failures = 0;
    const auto check = [&](bool success) { if (!success) ++failures; };
    // Mixed types must preserve parent/edge identity across the coarse draw's
    // first-instance offset, including sparse edges and maximum occupancy.
    ParentMeshDescriptors<std::array<int, 4>, 3> descriptors{};
    descriptors.parents = {{{0, 1, -1, 0}, {1, -1, 0, 1}, {-1, 0, 1, -1}}};
    const auto classify = [](const auto& parent, std::uint32_t edge) { return parent[edge]; };
    auto counts = descriptors.buildBridges(3, classify);
    check(counts.normal == 4 && counts.coarse == 4);
    const std::array<std::uint32_t, 8> expected{0, 3, 6, 9, 1, 4, 7, 10};
    for (std::size_t i = 0; i < expected.size(); ++i)
        check(descriptors.bridges[i] == expected[i]);
    for (int type : {0, 1})
    {
        for (auto& parent : descriptors.parents) parent.fill(type);
        counts = descriptors.buildBridges(3, classify);
        check(counts.normal == (type == 0 ? 12u : 0u));
        check(counts.coarse == (type == 1 ? 12u : 0u));
        for (std::uint32_t i = 0; i < 12; ++i) check(descriptors.bridges[i] == i);
    }
    counts = descriptors.buildBridges(0, classify);
    check(counts.normal == 0 && counts.coarse == 0);
    for (auto& parent : descriptors.parents) parent.fill(-1);
    counts = descriptors.buildBridges(3, classify);
    check(counts.normal == 0 && counts.coarse == 0);
    // Exact neighbor sample positions, independent of world position or pitch.
    constexpr std::array<std::array<int, 2>, 8> samples{{
        {1, 1}, {129, 1}, {257, 1}, {1, 129},
        {257, 129}, {1, 257}, {129, 257}, {257, 257}}};
    for (std::uint8_t selector = 0; selector < 8; ++selector)
    {
        const auto position = selector < 4 ? selector : selector + 1;
        check((position % 3) * 128 + 1 == samples[selector][0]);
        check((position / 3) * 128 + 1 == samples[selector][1]);
    }
    constexpr std::uint8_t ownCorners[]{0, 2, 7, 5};
    constexpr std::uint8_t diagonalCorners[]{7, 5, 0, 2};
    constexpr std::uint8_t edgeCorners[4][2]{{0, 3}, {1, 0}, {2, 1}, {3, 2}};
    constexpr std::uint8_t expectedEdgeSelectors[4][2][2]{
        {{2, 4}, {4, 7}}, {{6, 5}, {7, 6}},
        {{3, 0}, {5, 3}}, {{0, 1}, {1, 2}},
    };
    for (std::uint8_t corner = 0; corner < 4; ++corner)
    {
        check(terrainOwnCornerSelector(corner) == ownCorners[corner]);
        check(terrainDiagonalCornerSelector(corner) == diagonalCorners[corner]);
    }
    for (std::uint8_t edge = 0; edge < 4; ++edge)
        for (std::uint8_t half = 0; half < 2; ++half)
            for (std::uint8_t endpoint = 0; endpoint < 2; ++endpoint)
                check(terrainEdgeCornerSelector(edge, half, edgeCorners[edge][endpoint]) ==
                    expectedEdgeSelectors[edge][half][endpoint]);
    for (std::uint8_t edge = 0; edge < 4; ++edge)
        for (std::uint8_t half = 0; half < 2; ++half)
            for (std::uint8_t first = 0; first < 8; ++first)
                for (std::uint8_t second = 0; second < 8; ++second)
                {
                    const auto metadata = terrainBridgeMetadata(11, edge, half, first, second);
                    check((metadata & 7u) == first && ((metadata >> 3u) & 7u) == second);
                    check(((metadata >> 16u) & 255u) == 11);
                    check(((metadata >> 24u) & 3u) == edge && ((metadata >> 26u) & 1u) == half);
                }
    // Rotate the reported layout through every corner, including negative roots.
    for (const auto grid : {0LL, -3LL})
        for (std::uint8_t coarseQuadrant = 0; coarseQuadrant < 4; ++coarseQuadrant)
        {
            std::vector<Node> nodes(5);
            nodes[0].nodeId = {grid, grid, 0};
            for (std::uint8_t q = 0; q < 4; ++q)
            {
                nodes[0].children[q] = q + 1;
                nodes[q + 1].nodeId = {grid, grid, WorldGridQuadtreeLeafId::appendChild(0, q)};
            }
            const auto opposite = coarseQuadrant ^ 3;
            auto fine = nodes[opposite + 1].nodeId;
            fine.subdivisionPath = WorldGridQuadtreeLeafId::appendChild(fine.subdivisionPath, coarseQuadrant);
            constexpr std::array<std::uint8_t, 4> cornerForQuadrant{2, 3, 1, 0};
            const auto findRoot = [&](std::int64_t x, std::int64_t z) -> std::uint16_t {
                return x == grid && z == grid ? 0 : none;
            };
            const auto getNode = [&](std::uint16_t index) -> const Node& { return nodes[index]; };
            const auto corner = cornerForQuadrant[coarseQuadrant];
            check(terrainCoarseDiagonalNeighbor(fine, corner, none, findRoot, getNode) == coarseQuadrant + 1);
            // Subdivided coarse parents must not win merely because their tile remains cached.
            const auto firstChild = static_cast<std::uint16_t>(nodes.size());
            const auto parentId = nodes[coarseQuadrant + 1].nodeId;
            for (std::uint8_t q = 0; q < 4; ++q)
            {
                nodes[coarseQuadrant + 1].children[q] = firstChild + q;
                auto id = parentId;
                id.subdivisionPath = WorldGridQuadtreeLeafId::appendChild(id.subdivisionPath, q);
                nodes.push_back({id});
            }
            check(terrainCoarseDiagonalNeighbor(fine, corner, none, findRoot, getNode) == none);
            // Partial child allocation must also reject the cached parent.
            nodes[coarseQuadrant + 1].children[0] = none;
            check(terrainCoarseDiagonalNeighbor(fine, corner, none, findRoot, getNode) == none);
        }
    // Diagonal lookup across a base-root boundary, and an absent root.
    const Node root{{0, 0, 0}};
    const WorldGridQuadtreeLeafId fine{-1, -1, WorldGridQuadtreeLeafId::appendChild(0, 0)};
    const auto getRoot = [&](std::uint16_t) -> const Node& { return root; };
    check(terrainCoarseDiagonalNeighbor(fine, 2, none,
        [](std::int64_t x, std::int64_t z) -> std::uint16_t { return x == 0 && z == 0 ? 0 : none; }, getRoot) == 0);
    check(terrainCoarseDiagonalNeighbor(fine, 2, none,
        [](std::int64_t, std::int64_t) { return none; }, getRoot) == none);
    if (failures) std::cerr << failures << " corner neighbor checks failed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
