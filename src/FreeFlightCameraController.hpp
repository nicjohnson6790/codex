#pragma once

#include "CameraManager.hpp"
#include "GamepadInput.hpp"

class FreeFlightCameraController
{
public:
    void update(CameraManager::Camera& camera, const GamepadState& gamepadState, float deltaTimeSeconds);

private:
    bool m_rightStickWasPressed = false;
};
