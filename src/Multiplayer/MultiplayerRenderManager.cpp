#include "Multiplayer/MultiplayerRenderManager.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

void MultiplayerRenderManager::emitDraws(
    const MultiplayerManager& multiplayerManager,
    TriangleRenderer& triangleRenderer,
    WorldTextRenderer& worldTextRenderer) const
{
    for (const MultiplayerManager::RemoteEntity& entity : multiplayerManager.remoteEntities())
    {
        const Position markerPosition = entity.drawPosition.translated({ 0.0, 1.2, 0.0 });
        triangleRenderer.addTriangle(markerPosition, entity.yawRadians, entity.color);

        worldTextRenderer.addLineCentered(
            entity.drawPosition.translated({ 0.0, 2.8, 0.0 }),
            entity.personaName,
            WorldTextRenderer::Style{
                .baseColor = glm::vec4(entity.color, 1.0f),
                .strokeColor = glm::vec4(0.02f, 0.02f, 0.025f, 1.0f),
                .strokeWidth = 2.0f,
                .glowColor = glm::vec4(0.0f, 0.0f, 0.0f, 0.55f),
                .glowWidth = 3.0f,
                .glowOffset = glm::vec2(3.0f, -3.0f),
            });
    }
}
