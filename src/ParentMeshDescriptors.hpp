#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

// One graphics SSBO: fixed-capacity parent records followed by compact bridge
// references. A reference packs parent index in the high bits and edge in bits 0-1.
template<class Parent, std::size_t Capacity>
struct ParentMeshDescriptors
{
    std::array<Parent, Capacity> parents{};
    std::array<std::uint32_t, Capacity * 4> bridges{};

    struct Counts { std::uint32_t normal = 0, coarse = 0; };

    // classify(parent, edge): -1 absent, 0 normal, 1 coarse. Partition references
    // without sorting or duplicating the descriptors consumed by either draw.
    template<class Classify>
    Counts buildBridges(std::uint32_t count, Classify classify)
    {
        assert(count <= Capacity);
        Counts result{};
        std::uint32_t output = 0;
        for (int type = 0; type < 2; ++type)
        {
            for (std::uint32_t parent = 0; parent < count; ++parent)
                for (std::uint32_t edge = 0; edge < 4; ++edge)
                    if (classify(parents[parent], edge) == type)
                        bridges[output++] = (parent << 2) | edge;
            if (type == 0) result.normal = output;
        }
        result.coarse = output - result.normal;
        return result;
    }
};
