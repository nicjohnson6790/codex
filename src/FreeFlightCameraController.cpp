#define GLM_ENABLE_EXPERIMENTAL

#include "FreeFlightCameraController.hpp"

#include "AppConfig.hpp"

#include <glm/geometric.hpp>
#include <glm/gtx/rotate_vector.hpp>

#include <cmath>

void FreeFlightCameraController::update(CameraManager::Camera& camera, const GamepadState& gamepadState, float deltaTimeSeconds)
{
    if (!gamepadState.hasGamepad)
    {
        m_rightStickWasPressed = false;
        return;
    }

    glm::dvec3 cameraRight = glm::normalize(glm::cross(camera.forward, camera.up));
    if (glm::length(cameraRight) <= 0.00001)
    {
        cameraRight = { 1.0, 0.0, 0.0 };
    }
    camera.up = glm::normalize(glm::cross(cameraRight, camera.forward));

    const double yawRadians = -static_cast<double>(gamepadState.rightX) * AppConfig::Camera::kLookSpeedRadians * static_cast<double>(deltaTimeSeconds);
    const double pitchRadians = -static_cast<double>(gamepadState.rightY) * AppConfig::Camera::kLookSpeedRadians * static_cast<double>(deltaTimeSeconds);
    const double rollInput = (gamepadState.rightShoulder ? 1.0 : 0.0) - (gamepadState.leftShoulder ? 1.0 : 0.0);
    const double rollRadians = rollInput * AppConfig::Camera::kRollSpeedRadians * static_cast<double>(deltaTimeSeconds);

    if (std::fabs(yawRadians) > 0.0)
    {
        camera.forward = glm::normalize(glm::rotate(camera.forward, yawRadians, camera.up));
        cameraRight = glm::normalize(glm::cross(camera.forward, camera.up));
    }

    if (std::fabs(pitchRadians) > 0.0)
    {
        camera.forward = glm::normalize(glm::rotate(camera.forward, pitchRadians, cameraRight));
        camera.up = glm::normalize(glm::rotate(camera.up, pitchRadians, cameraRight));
    }

    if (std::fabs(rollRadians) > 0.0)
    {
        camera.up = glm::normalize(glm::rotate(camera.up, rollRadians, camera.forward));
    }

    cameraRight = glm::normalize(glm::cross(camera.forward, camera.up));
    camera.up = glm::normalize(glm::cross(cameraRight, camera.forward));

    const double altitude = std::abs(camera.position.localPosition().y);
    const double movementSpeed = AppConfig::Camera::kMoveSpeed * (1.0 + (altitude * AppConfig::Camera::kAltitudeSpeedScale));
    const glm::dvec3 translation =
        (cameraRight * static_cast<double>(gamepadState.leftX)) +
        (camera.forward * static_cast<double>(-gamepadState.leftY)) +
        (AppConfig::Camera::kWorldUp * static_cast<double>(gamepadState.rightTrigger - gamepadState.leftTrigger));
    camera.position.addLocalOffset(translation * (movementSpeed * static_cast<double>(deltaTimeSeconds)));

    if (gamepadState.rightStickPressed && !m_rightStickWasPressed)
    {
        if (std::fabs(glm::dot(camera.forward, AppConfig::Camera::kWorldUp)) < 0.999)
        {
            cameraRight = glm::normalize(glm::cross(camera.forward, AppConfig::Camera::kWorldUp));
            camera.up = glm::normalize(glm::cross(cameraRight, camera.forward));
        }
        else
        {
            camera.forward = AppConfig::Camera::kFallbackForward;
            camera.up = AppConfig::Camera::kWorldUp;
        }
    }

    m_rightStickWasPressed = gamepadState.rightStickPressed;
}
