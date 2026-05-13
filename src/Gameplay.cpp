#include "Gameplay.hpp"

#include "AppConfig.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/geometric.hpp>

namespace
{
constexpr double kCollisionPageSize = static_cast<double>(FoliageConfig::kPageSizeMeters);
constexpr std::int64_t kPagesPerWorldCell = Position::kCellSize / FoliageConfig::kPageSizeMeters;
constexpr std::uint32_t kNearbyFoliageFlagsShift = 16u;
constexpr double kTreeTrunkRadius = 0.35;

std::int64_t floorDiv(std::int64_t value, std::int64_t divisor)
{
    std::int64_t quotient = value / divisor;
    const std::int64_t remainder = value % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0)))
    {
        --quotient;
    }
    return quotient;
}

std::int64_t floorToInt64(double value)
{
    return static_cast<std::int64_t>(std::floor(value));
}

bool decodedInstanceResident(const DecodedNearbyFoliageInstance& instance)
{
    return ((instance.packedMeta >> kNearbyFoliageFlagsShift) & NearbyFoliageInstance_Resident) != 0u;
}

glm::dvec2 normalizedOrZero(glm::dvec2 value)
{
    const double length = glm::length(value);
    return length > 0.00001 ? value / length : glm::dvec2(0.0);
}

glm::dvec3 normalizedOrFallback(glm::dvec3 value, const glm::dvec3& fallback)
{
    const double length = glm::length(value);
    return length > 0.00001 ? value / length : fallback;
}

double shortestAngleDelta(double fromRadians, double toRadians)
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kTwoPi = kPi * 2.0;
    double delta = std::fmod(toRadians - fromRadians, kTwoPi);
    if (delta > kPi)
    {
        delta -= kTwoPi;
    }
    else if (delta < -kPi)
    {
        delta += kTwoPi;
    }
    return delta;
}
}

PlayerMoveIntent PlayerController::poll(const GamepadState& gamepadState) const
{
    PlayerMoveIntent intent{};

    int keyCount = 0;
    const bool* keys = SDL_GetKeyboardState(&keyCount);
    auto keyDown = [&](SDL_Scancode scancode) {
        return static_cast<int>(scancode) < keyCount && keys[scancode];
    };

    if (keys != nullptr)
    {
        intent.move.y += keyDown(SDL_SCANCODE_W) ? 1.0 : 0.0;
        intent.move.y -= keyDown(SDL_SCANCODE_S) ? 1.0 : 0.0;
        intent.move.x += keyDown(SDL_SCANCODE_D) ? 1.0 : 0.0;
        intent.move.x -= keyDown(SDL_SCANCODE_A) ? 1.0 : 0.0;
        intent.sprint = keyDown(SDL_SCANCODE_LSHIFT) || keyDown(SDL_SCANCODE_RSHIFT);
    }

    if (gamepadState.hasGamepad)
    {
        intent.move.x += static_cast<double>(gamepadState.leftX);
        intent.move.y += static_cast<double>(-gamepadState.leftY);
        intent.look.x = static_cast<double>(gamepadState.rightX);
        intent.look.y = static_cast<double>(gamepadState.rightY);
        intent.sprint = intent.sprint || gamepadState.rightShoulder;
    }

    intent.move = normalizedOrZero(intent.move);
    return intent;
}

void CollisionManager::updateAroundPlayer(
    const Position& playerPosition,
    std::uint64_t frameIndex,
    WorldGridQuadtreeHeightmapManager& heightmapManager,
    WorldGridFoliageManager& foliageManager,
    NearbyFoliageRenderer& nearbyFoliageRenderer,
    QuadtreeMeshRenderer& meshRenderer)
{
    const glm::dvec2 page = pageCoordinatesForWorldPosition(playerPosition.worldPosition());
    const std::int64_t centerPageX = floorToInt64(page.x);
    const std::int64_t centerPageZ = floorToInt64(page.y);

    for (std::int64_t dz = -1; dz <= 1; ++dz)
    {
        for (std::int64_t dx = -1; dx <= 1; ++dx)
        {
            const WorldGridQuadtreeLeafId key = leafIdForPage(centerPageX + dx, centerPageZ + dz);
            CollisionTile& tile = touchTile(key, frameIndex);

            if (!tile.heightReady && heightmapManager.makeCpuResident(key, meshRenderer))
            {
                CpuResidentHeightmapView heightmapView{};
                if (heightmapManager.tryGetCpuResidentHeightmap(key, heightmapView))
                {
                    std::copy(heightmapView.samples.begin(), heightmapView.samples.end(), tile.heightmap.begin());
                    tile.heightReady = true;
                }
            }

            std::uint16_t terrainSliceIndex = 0;
            if (heightmapManager.getResidentSliceIndex(key, terrainSliceIndex))
            {
                if (foliageManager.makeResident(key, key, terrainSliceIndex))
                {
                    FoliageReadyPageInfo pageInfo{};
                    if (foliageManager.getReadyPageInfo(key, pageInfo) &&
                        nearbyFoliageRenderer.makeResident(key, pageInfo, frameIndex))
                    {
                        NearbyFoliageRenderer::CpuResidentPageView foliageView{};
                        if (nearbyFoliageRenderer.tryGetCpuResidentPage(key, foliageView) &&
                            (!tile.treesReady || tile.treeContentVersion != foliageView.contentVersion))
                        {
                            std::copy(foliageView.instances.begin(), foliageView.instances.end(), tile.treeGrid.begin());
                            tile.treeLiveCount = foliageView.liveCount;
                            tile.treeContentVersion = foliageView.contentVersion;
                            tile.treesReady = true;
                        }
                    }
                }
            }
        }
    }
}

CollisionManager::GroundSample CollisionManager::sampleGround(const Position& worldPosition) const
{
    const WorldGridQuadtreeLeafId key = leafIdForWorldPosition(worldPosition.worldPosition());
    const CollisionTile* tile = findTile(key);
    if (tile == nullptr || !tile->heightReady)
    {
        return {};
    }

    const auto [minCorner, maxCorner] = worldGridQuadtreeLeafBounds(key);
    const glm::dvec3 minWorld = minCorner.worldPosition();
    const glm::dvec3 maxWorld = maxCorner.worldPosition();
    const double tileSize = maxWorld.x - minWorld.x;
    const double step = tileSize / static_cast<double>(AppConfig::Terrain::kHeightmapLeafIntervalCount);
    const double sampleX = ((worldPosition.worldPosition().x - minWorld.x) / step) +
        static_cast<double>(AppConfig::Terrain::kHeightmapLeafHalo);
    const double sampleZ = ((worldPosition.worldPosition().z - minWorld.z) / step) +
        static_cast<double>(AppConfig::Terrain::kHeightmapLeafHalo);
    const double clampedX = std::clamp(sampleX, 0.0, static_cast<double>(AppConfig::Terrain::kHeightmapResolution - 1u));
    const double clampedZ = std::clamp(sampleZ, 0.0, static_cast<double>(AppConfig::Terrain::kHeightmapResolution - 1u));
    const std::uint32_t x0 = static_cast<std::uint32_t>(std::floor(clampedX));
    const std::uint32_t z0 = static_cast<std::uint32_t>(std::floor(clampedZ));
    const std::uint32_t x1 = std::min(x0 + 1u, AppConfig::Terrain::kHeightmapResolution - 1u);
    const std::uint32_t z1 = std::min(z0 + 1u, AppConfig::Terrain::kHeightmapResolution - 1u);
    const double tx = clampedX - static_cast<double>(x0);
    const double tz = clampedZ - static_cast<double>(z0);

    auto sample = [&](std::uint32_t x, std::uint32_t z) {
        return static_cast<double>(tile->heightmap[
            static_cast<std::size_t>(z) * AppConfig::Terrain::kHeightmapResolution + x]);
    };

    const double h00 = sample(x0, z0);
    const double h10 = sample(x1, z0);
    const double h01 = sample(x0, z1);
    const double h11 = sample(x1, z1);
    const double hx0 = h00 + ((h10 - h00) * tx);
    const double hx1 = h01 + ((h11 - h01) * tx);
    const double height = hx0 + ((hx1 - hx0) * tz);

    const std::uint32_t nx0 = x0 > 0 ? x0 - 1u : x0;
    const std::uint32_t nx1 = std::min(x0 + 1u, AppConfig::Terrain::kHeightmapResolution - 1u);
    const std::uint32_t nz0 = z0 > 0 ? z0 - 1u : z0;
    const std::uint32_t nz1 = std::min(z0 + 1u, AppConfig::Terrain::kHeightmapResolution - 1u);
    const double dhdx = (sample(nx1, z0) - sample(nx0, z0)) / (static_cast<double>(nx1 - nx0) * step);
    const double dhdz = (sample(x0, nz1) - sample(x0, nz0)) / (static_cast<double>(nz1 - nz0) * step);

    return {
        .height = height,
        .normal = normalizedOrFallback(glm::dvec3(-dhdx, 1.0, -dhdz), AppConfig::Camera::kWorldUp),
        .valid = true,
    };
}

glm::dvec3 CollisionManager::resolveTreeCollisions(const Position& position, double playerRadius) const
{
    glm::dvec3 correction{ 0.0 };
    glm::dvec3 world = position.worldPosition();
    const double queryRadius = playerRadius + kTreeTrunkRadius;
    const std::int64_t minPageX = floorToInt64((world.x - queryRadius) / kCollisionPageSize);
    const std::int64_t maxPageX = floorToInt64((world.x + queryRadius) / kCollisionPageSize);
    const std::int64_t minPageZ = floorToInt64((world.z - queryRadius) / kCollisionPageSize);
    const std::int64_t maxPageZ = floorToInt64((world.z + queryRadius) / kCollisionPageSize);

    for (std::int64_t pageZ = minPageZ; pageZ <= maxPageZ; ++pageZ)
    {
        for (std::int64_t pageX = minPageX; pageX <= maxPageX; ++pageX)
        {
            const WorldGridQuadtreeLeafId key = leafIdForPage(pageX, pageZ);
            const CollisionTile* tile = findTile(key);
            if (tile == nullptr || !tile->treesReady)
            {
                continue;
            }

            const auto [minCorner, maxCorner] = worldGridQuadtreeLeafBounds(key);
            (void)maxCorner;
            const glm::dvec3 pageOrigin = minCorner.worldPosition();
            const double localX = world.x - pageOrigin.x;
            const double localZ = world.z - pageOrigin.z;
            const double cellSize = static_cast<double>(FoliageConfig::kCandidateCellSizeMeters);
            const int minCellX = std::max(0, static_cast<int>(std::floor((localX - queryRadius) / cellSize)));
            const int maxCellX = std::min(
                static_cast<int>(FoliageConfig::kCandidateGridResolution - 1u),
                static_cast<int>(std::floor((localX + queryRadius) / cellSize)));
            const int minCellZ = std::max(0, static_cast<int>(std::floor((localZ - queryRadius) / cellSize)));
            const int maxCellZ = std::min(
                static_cast<int>(FoliageConfig::kCandidateGridResolution - 1u),
                static_cast<int>(std::floor((localZ + queryRadius) / cellSize)));

            for (int cellZ = minCellZ; cellZ <= maxCellZ; ++cellZ)
            {
                for (int cellX = minCellX; cellX <= maxCellX; ++cellX)
                {
                    const std::uint32_t candidateSlot =
                        static_cast<std::uint32_t>(cellZ) * FoliageConfig::kCandidateGridResolution +
                        static_cast<std::uint32_t>(cellX);
                    const DecodedNearbyFoliageInstance& instance = tile->treeGrid[candidateSlot];
                    if (!decodedInstanceResident(instance))
                    {
                        continue;
                    }

                    const glm::dvec2 delta{
                        world.x - (pageOrigin.x + static_cast<double>(instance.localX)),
                        world.z - (pageOrigin.z + static_cast<double>(instance.localZ)),
                    };
                    const double distance = glm::length(delta);
                    if (distance <= 0.00001 || distance >= queryRadius)
                    {
                        continue;
                    }

                    const glm::dvec2 push = (delta / distance) * (queryRadius - distance);
                    world.x += push.x;
                    world.z += push.y;
                    correction.x += push.x;
                    correction.z += push.y;
                }
            }
        }
    }

    return correction;
}

std::uint32_t CollisionManager::readyTileCount() const
{
    std::uint32_t count = 0;
    for (const CollisionTile& tile : m_tiles)
    {
        if (tile.used && tile.heightReady)
        {
            ++count;
        }
    }
    return count;
}

CollisionManager::CollisionTile& CollisionManager::touchTile(
    const WorldGridQuadtreeLeafId& key,
    std::uint64_t frameIndex)
{
    for (CollisionTile& tile : m_tiles)
    {
        if (tile.used && tile.key == key)
        {
            tile.lastUsedFrame = frameIndex;
            return tile;
        }
    }

    CollisionTile* reusable = nullptr;
    for (CollisionTile& tile : m_tiles)
    {
        if (!tile.used)
        {
            reusable = &tile;
            break;
        }
        if (reusable == nullptr || tile.lastUsedFrame < reusable->lastUsedFrame)
        {
            reusable = &tile;
        }
    }

    *reusable = {};
    reusable->used = true;
    reusable->key = key;
    reusable->lastUsedFrame = frameIndex;
    return *reusable;
}

const CollisionManager::CollisionTile* CollisionManager::findTile(const WorldGridQuadtreeLeafId& key) const
{
    for (const CollisionTile& tile : m_tiles)
    {
        if (tile.used && tile.key == key)
        {
            return &tile;
        }
    }

    return nullptr;
}

WorldGridQuadtreeLeafId CollisionManager::leafIdForWorldPosition(const glm::dvec3& worldPosition)
{
    const glm::dvec2 page = pageCoordinatesForWorldPosition(worldPosition);
    return leafIdForPage(floorToInt64(page.x), floorToInt64(page.y));
}

WorldGridQuadtreeLeafId CollisionManager::leafIdForPage(std::int64_t pageX, std::int64_t pageZ)
{
    const std::int64_t gridX = floorDiv(pageX, kPagesPerWorldCell);
    const std::int64_t gridY = floorDiv(pageZ, kPagesPerWorldCell);
    const std::uint32_t localPageX = static_cast<std::uint32_t>(pageX - (gridX * kPagesPerWorldCell));
    const std::uint32_t localPageZ = static_cast<std::uint32_t>(pageZ - (gridY * kPagesPerWorldCell));

    std::uint64_t path = 0;
    for (int bitIndex = 10; bitIndex >= 0; --bitIndex)
    {
        const bool highX = ((localPageX >> bitIndex) & 1u) != 0u;
        const bool highZ = ((localPageZ >> bitIndex) & 1u) != 0u;
        const std::uint32_t quadrant = highX ? (highZ ? 0u : 2u) : (highZ ? 1u : 3u);
        path = WorldGridQuadtreeLeafId::appendChild(path, quadrant);
    }

    return {
        .gridX = gridX,
        .gridY = gridY,
        .subdivisionPath = path,
    };
}

glm::dvec2 CollisionManager::pageCoordinatesForWorldPosition(const glm::dvec3& worldPosition)
{
    return {
        std::floor(worldPosition.x / kCollisionPageSize),
        std::floor(worldPosition.z / kCollisionPageSize),
    };
}

void CharacterMotor::update(
    PlayerPawn& pawn,
    const PlayerMoveIntent& intent,
    const CameraManager::Camera& camera,
    const CollisionManager& collisionManager,
    float deltaTimeSeconds) const
{
    const CollisionManager::GroundSample ground = collisionManager.sampleGround(pawn.position);
    if (!ground.valid)
    {
        pawn.velocity = { 0.0, 0.0, 0.0 };
        pawn.grounded = false;
        return;
    }

    glm::dvec3 cameraForward = normalizedOrFallback(
        glm::dvec3(camera.forward.x, 0.0, camera.forward.z),
        glm::dvec3(0.0, 0.0, -1.0));
    glm::dvec3 cameraRight = normalizedOrFallback(
        glm::cross(cameraForward, AppConfig::Camera::kWorldUp),
        glm::dvec3(1.0, 0.0, 0.0));

    const glm::dvec3 desiredDirection = normalizedOrFallback(
        (cameraRight * intent.move.x) + (cameraForward * intent.move.y),
        glm::dvec3(0.0));
    const double targetSpeed = intent.sprint ? kSprintSpeed : kWalkSpeed;
    const glm::dvec3 targetHorizontalVelocity = desiredDirection * targetSpeed;

    glm::dvec3 horizontalVelocity{ pawn.velocity.x, 0.0, pawn.velocity.z };
    const double accelerationStep = kAcceleration * static_cast<double>(deltaTimeSeconds);
    const glm::dvec3 deltaVelocity = targetHorizontalVelocity - horizontalVelocity;
    const double deltaLength = glm::length(deltaVelocity);
    if (deltaLength > accelerationStep && deltaLength > 0.00001)
    {
        horizontalVelocity += (deltaVelocity / deltaLength) * accelerationStep;
    }
    else
    {
        horizontalVelocity = targetHorizontalVelocity;
    }

    pawn.velocity.x = horizontalVelocity.x;
    pawn.velocity.z = horizontalVelocity.z;
    pawn.velocity.y -= kGravity * static_cast<double>(deltaTimeSeconds);

    pawn.position.addLocalOffset(pawn.velocity * static_cast<double>(deltaTimeSeconds));
    pawn.position.addLocalOffset(collisionManager.resolveTreeCollisions(pawn.position, pawn.radius));

    const CollisionManager::GroundSample newGround = collisionManager.sampleGround(pawn.position);
    if (newGround.valid)
    {
        const double currentY = pawn.position.localPosition().y;
        const double groundDelta = currentY - newGround.height;
        if (groundDelta <= kGroundSnapDistance || currentY < newGround.height)
        {
            pawn.position.setLocalY(newGround.height);
            pawn.velocity.y = 0.0;
            pawn.grounded = true;
        }
        else
        {
            pawn.grounded = false;
        }
    }

    if (glm::length(horizontalVelocity) > 0.01)
    {
        pawn.yawRadians = std::atan2(horizontalVelocity.x, horizontalVelocity.z);
    }
}

void FollowCameraController::snapToCamera(const CameraManager::Camera& sourceCamera, PlayerPawn& pawn)
{
    const glm::dvec3 cameraWorld = sourceCamera.position.worldPosition();
    const glm::dvec3 forward = normalizedOrFallback(sourceCamera.forward, AppConfig::Camera::kFallbackForward);
    const glm::dvec3 target = cameraWorld + (forward * m_distance);
    pawn.position = Position(0, 0, target - glm::dvec3(0.0, m_heightOffset, 0.0));

    const glm::dvec3 back = forward;
    m_orbitPitchRadians = std::clamp(-std::asin(std::clamp(back.y, -1.0, 1.0)), -1.05, 1.05);
    m_orbitYawRadians = std::atan2(back.x, back.z);

    const glm::dvec3 horizontalForward = normalizedOrFallback(
        glm::dvec3(forward.x, 0.0, forward.z),
        glm::dvec3(0.0, 0.0, -1.0));
    pawn.yawRadians = std::atan2(horizontalForward.x, horizontalForward.z);
    pawn.velocity = glm::dvec3(0.0);
}

void FollowCameraController::update(
    CameraManager::Camera& camera,
    const PlayerPawn& pawn,
    const CollisionManager& collisionManager,
    const PlayerMoveIntent& intent,
    float deltaTimeSeconds)
{
    constexpr double kLookSpeed = 2.6;
    constexpr double kPitchLimit = 1.05;
    constexpr double kBehindPlayerDecayRate = 3.0;
    constexpr double kGroundClearance = 0.35;

    m_orbitYawRadians -= intent.look.x * kLookSpeed * static_cast<double>(deltaTimeSeconds);
    m_orbitPitchRadians = std::clamp(
        m_orbitPitchRadians + (intent.look.y * kLookSpeed * static_cast<double>(deltaTimeSeconds)),
        -kPitchLimit,
        kPitchLimit);

    if (glm::length(intent.move) > 0.0001)
    {
        const double blend = 1.0 - std::exp(-kBehindPlayerDecayRate * static_cast<double>(deltaTimeSeconds));
        m_orbitYawRadians += shortestAngleDelta(m_orbitYawRadians, pawn.yawRadians) * blend;
    }

    const glm::dvec3 target = pawn.position.worldPosition() + glm::dvec3(0.0, m_heightOffset, 0.0);
    const double cosPitch = std::cos(m_orbitPitchRadians);
    const glm::dvec3 back{
        std::sin(m_orbitYawRadians) * cosPitch,
        std::sin(-m_orbitPitchRadians),
        std::cos(m_orbitYawRadians) * cosPitch,
    };

    glm::dvec3 cameraWorld = target - (back * m_distance);
    const CollisionManager::GroundSample cameraGround = collisionManager.sampleGround(Position(0, 0, cameraWorld));
    if (cameraGround.valid)
    {
        cameraWorld.y = std::max(cameraWorld.y, cameraGround.height + kGroundClearance);
    }

    const glm::dvec3 forward = normalizedOrFallback(target - cameraWorld, glm::dvec3(0.0, 0.0, -1.0));
    const glm::dvec3 right = normalizedOrFallback(glm::cross(forward, AppConfig::Camera::kWorldUp), glm::dvec3(1.0, 0.0, 0.0));

    camera.position = Position(0, 0, cameraWorld);
    camera.forward = forward;
    camera.up = normalizedOrFallback(glm::cross(right, forward), AppConfig::Camera::kWorldUp);
}
