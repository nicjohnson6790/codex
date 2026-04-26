#pragma once

#include "RenderEngines.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <cstdint>

class WorldGridQuadtreeDebugRenderer
{
public:
    void appendNodeBorder(RenderEngines& renderEngines, const WorldGridQuadtreeLeafId& nodeId, std::uint32_t maxDepth) const;
};
