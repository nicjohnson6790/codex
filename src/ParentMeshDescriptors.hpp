#pragma once

#include <array>
#include <cstddef>

// Fixed-capacity graphics SSBO; only populated parent records are uploaded.
template<class Parent, std::size_t Capacity>
struct ParentMeshDescriptors
{
    std::array<Parent, Capacity> parents;
};
