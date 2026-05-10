#pragma once

#include "CameraManager.hpp"
#include "FoliageTypes.hpp"
#include "GamepadInput.hpp"
#include "NearbyFoliageRenderer.hpp"
#include "Position.hpp"
#include "QuadtreeMeshRenderer.hpp"
#include "WorldGridFoliageManager.hpp"
#include "WorldGridQuadtreeHeightmapManager.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <array>
#include <cstdint>
#include <optional>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

struct PlayerPawn
{
    Position position{ 0, 0, { 0.0, 300.0, 0.0 } };
    glm::dvec3 velocity{ 0.0 };
    double yawRadians = 0.0;
    bool grounded = false;
    double radius = 0.45;
    double height = 1.8;
};

struct PlayerMoveIntent
{
    glm::dvec2 move{ 0.0 };
    glm::dvec2 look{ 0.0 };
    bool sprint = false;
};

class PlayerController
{
public:
    [[nodiscard]] PlayerMoveIntent poll(const GamepadState& gamepadState) const;
};

class CollisionManager
{
public:
    struct GroundSample
    {
        double height = 0.0;
        glm::dvec3 normal{ 0.0, 1.0, 0.0 };
        bool valid = false;
    };

    void updateAroundPlayer(
        const Position& playerPosition,
        std::uint64_t frameIndex,
        WorldGridQuadtreeHeightmapManager& heightmapManager,
        WorldGridFoliageManager& foliageManager,
        NearbyFoliageRenderer& nearbyFoliageRenderer,
        QuadtreeMeshRenderer& meshRenderer);

    [[nodiscard]] GroundSample sampleGround(const Position& worldPosition) const;
    [[nodiscard]] glm::dvec3 resolveTreeCollisions(const Position& position, double playerRadius) const;
    [[nodiscard]] std::uint32_t readyTileCount() const;

private:
    struct CollisionTile
    {
        WorldGridQuadtreeLeafId key{};
        std::array<float, QuadtreeMeshRenderer::kHeightmapSliceSampleCount> heightmap{};
        std::array<DecodedNearbyFoliageInstance, FoliageConfig::kCandidateSlotCount> treeGrid{};
        bool used = false;
        bool heightReady = false;
        bool treesReady = false;
        std::uint64_t lastUsedFrame = 0;
        std::uint32_t treeContentVersion = 0;
        std::uint16_t treeLiveCount = 0;
    };

    [[nodiscard]] CollisionTile& touchTile(const WorldGridQuadtreeLeafId& key, std::uint64_t frameIndex);
    [[nodiscard]] const CollisionTile* findTile(const WorldGridQuadtreeLeafId& key) const;
    [[nodiscard]] static WorldGridQuadtreeLeafId leafIdForWorldPosition(const glm::dvec3& worldPosition);
    [[nodiscard]] static WorldGridQuadtreeLeafId leafIdForPage(std::int64_t pageX, std::int64_t pageZ);
    [[nodiscard]] static glm::dvec2 pageCoordinatesForWorldPosition(const glm::dvec3& worldPosition);

    static constexpr std::uint32_t kTileCacheSize = 16;
    std::array<CollisionTile, kTileCacheSize> m_tiles{};
};

class CharacterMotor
{
public:
    void update(
        PlayerPawn& pawn,
        const PlayerMoveIntent& intent,
        const CameraManager::Camera& camera,
        const CollisionManager& collisionManager,
        float deltaTimeSeconds) const;

private:
    static constexpr double kWalkSpeed = 8.0;
    static constexpr double kSprintSpeed = 16.0;
    static constexpr double kAcceleration = 32.0;
    static constexpr double kGravity = 28.0;
    static constexpr double kGroundSnapDistance = 2.0;
};

class FollowCameraController
{
public:
    void update(
        CameraManager::Camera& camera,
        const PlayerPawn& pawn,
        const PlayerMoveIntent& intent,
        float deltaTimeSeconds);

private:
    double m_orbitYawRadians = 0.0;
    double m_orbitPitchRadians = -0.34;
    double m_distance = 8.0;
    double m_heightOffset = 2.2;
};
