#pragma once

#include "CameraManager.hpp"
#include "GamepadInput.hpp"

class CityBuilderCameraController
{
public:
    void constrain(CameraManager::Camera& camera) const;
    void update(CameraManager::Camera& camera, const GamepadState& gamepadState, float deltaTimeSeconds) const;
};
