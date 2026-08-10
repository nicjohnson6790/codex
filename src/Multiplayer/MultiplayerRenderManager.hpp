#pragma once

#include "Multiplayer/MultiplayerManager.hpp"
#include "TriangleRenderer.hpp"
#include "WorldTextRenderer.hpp"

class MultiplayerRenderManager
{
public:
    void emitDraws(
        const MultiplayerManager& multiplayerManager,
        TriangleRenderer& triangleRenderer,
        WorldTextRenderer& worldTextRenderer) const;
};
