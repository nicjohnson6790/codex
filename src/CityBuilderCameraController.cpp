#include "CityBuilderCameraController.hpp"

#include "AppConfig.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace
{
struct Angles
{
    double yaw;
    double pitch;
};

Angles cameraAngles(const CameraManager::Camera& camera)
{
    const glm::dvec3 forward = glm::normalize(camera.forward);
    glm::dvec3 heading{forward.x, 0.0, forward.z};
    if (glm::dot(heading, heading) < 1.0e-16)
    {
        // At straight down, the camera's screen-up direction retains its heading.
        heading = {camera.up.x, 0.0, camera.up.z};
        if (forward.y > 0.0)
        {
            heading = -heading;
        }
        if (glm::dot(heading, heading) < 1.0e-16)
        {
            heading = AppConfig::Camera::kFallbackForward;
        }
    }
    return {std::atan2(-heading.x, -heading.z),
        std::clamp(std::asin(std::clamp(forward.y, -1.0, 1.0)), -std::numbers::pi / 2.0, 0.0)};
}

void applyAngles(CameraManager::Camera& camera, const Angles& angles)
{
    const glm::dvec3 heading{-std::sin(angles.yaw), 0.0, -std::cos(angles.yaw)};
    camera.forward = heading * std::cos(angles.pitch) + AppConfig::Camera::kWorldUp * std::sin(angles.pitch);
    camera.up = -heading * std::sin(angles.pitch) + AppConfig::Camera::kWorldUp * std::cos(angles.pitch);
}
}

void CityBuilderCameraController::constrain(CameraManager::Camera& camera) const
{
    applyAngles(camera, cameraAngles(camera));
}

void CityBuilderCameraController::update(CameraManager::Camera& camera, const GamepadState& gamepadState, float deltaTimeSeconds) const
{
    Angles angles = cameraAngles(camera);
    if (!gamepadState.hasGamepad)
    {
        applyAngles(camera, angles);
        return;
    }

    const double dt = static_cast<double>(deltaTimeSeconds);
    angles.yaw -= static_cast<double>(gamepadState.rightX) * AppConfig::Camera::kLookSpeedRadians * dt;
    angles.pitch = std::clamp(angles.pitch - static_cast<double>(gamepadState.rightY) * AppConfig::Camera::kLookSpeedRadians * dt,
        -std::numbers::pi / 2.0, 0.0);
    applyAngles(camera, angles);

    const glm::dvec3 heading{-std::sin(angles.yaw), 0.0, -std::cos(angles.yaw)};
    const glm::dvec3 right = glm::cross(heading, AppConfig::Camera::kWorldUp);
    const double altitude = std::abs(camera.position.localPosition().y);
    const double speed = AppConfig::Camera::kMoveSpeed * (1.0 + altitude * AppConfig::Camera::kAltitudeSpeedScale);
    const glm::dvec3 translation = right * static_cast<double>(gamepadState.leftX)
        - heading * static_cast<double>(gamepadState.leftY)
        + AppConfig::Camera::kWorldUp * static_cast<double>(gamepadState.rightTrigger - gamepadState.leftTrigger);
    camera.position.addLocalOffset(translation * (speed * dt));
}
