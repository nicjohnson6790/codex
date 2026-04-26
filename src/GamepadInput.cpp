#include "GamepadInput.hpp"

#include "AppConfig.hpp"

#include <glm/common.hpp>

#include <cmath>

namespace
{
float clamp01(float value)
{
    return glm::clamp(value, 0.0f, 1.0f);
}

float normalizeAxisValue(Sint16 value)
{
    const float normalized = glm::clamp(static_cast<float>(value) / 32767.0f, -1.0f, 1.0f);
    if (std::fabs(normalized) < AppConfig::Input::kAxisDeadzone)
    {
        return 0.0f;
    }

    const float sign = normalized < 0.0f ? -1.0f : 1.0f;
    const float scaled = (std::fabs(normalized) - AppConfig::Input::kAxisDeadzone) / (1.0f - AppConfig::Input::kAxisDeadzone);
    return sign * clamp01(scaled);
}

float normalizeTriggerValue(Sint16 value)
{
    const float normalized = glm::clamp(static_cast<float>(value) / 32767.0f, 0.0f, 1.0f);
    if (normalized < AppConfig::Input::kTriggerDeadzone)
    {
        return 0.0f;
    }
    return clamp01((normalized - AppConfig::Input::kTriggerDeadzone) / (1.0f - AppConfig::Input::kTriggerDeadzone));
}
}

GamepadInput::~GamepadInput()
{
    shutdown();
}

void GamepadInput::initialize()
{
    openFirstAvailableGamepad();
}

void GamepadInput::shutdown()
{
    closeGamepad();
}

void GamepadInput::handleEvent(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_GAMEPAD_ADDED)
    {
        if (m_gamepad == nullptr)
        {
            openFirstAvailableGamepad();
        }
    }
    else if (event.type == SDL_EVENT_GAMEPAD_REMOVED)
    {
        if (m_gamepad != nullptr && SDL_GetGamepadID(m_gamepad) == event.gdevice.which)
        {
            closeGamepad();
            openFirstAvailableGamepad();
        }
    }
}

GamepadState GamepadInput::pollState() const
{
    if (m_gamepad == nullptr)
    {
        return {};
    }

    return {
        .leftX = normalizeAxisValue(SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_LEFTX)),
        .leftY = normalizeAxisValue(SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_LEFTY)),
        .rightX = normalizeAxisValue(SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_RIGHTX)),
        .rightY = normalizeAxisValue(SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_RIGHTY)),
        .leftTrigger = normalizeTriggerValue(SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)),
        .rightTrigger = normalizeTriggerValue(SDL_GetGamepadAxis(m_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)),
        .leftShoulder = SDL_GetGamepadButton(m_gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER),
        .rightShoulder = SDL_GetGamepadButton(m_gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER),
        .rightStickPressed = SDL_GetGamepadButton(m_gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK),
        .hasGamepad = true,
    };
}

std::string_view GamepadInput::gamepadName() const
{
    if (m_gamepad == nullptr)
    {
        return "Not connected";
    }

    const char* name = SDL_GetGamepadName(m_gamepad);
    return name != nullptr ? std::string_view(name) : std::string_view("Connected");
}

bool GamepadInput::hasGamepad() const
{
    return m_gamepad != nullptr;
}

void GamepadInput::openFirstAvailableGamepad()
{
    if (m_gamepad != nullptr)
    {
        return;
    }

    int gamepadCount = 0;
    SDL_JoystickID* gamepads = SDL_GetGamepads(&gamepadCount);
    if (gamepads == nullptr)
    {
        return;
    }

    for (int index = 0; index < gamepadCount; ++index)
    {
        SDL_Gamepad* candidate = SDL_OpenGamepad(gamepads[index]);
        if (candidate != nullptr)
        {
            m_gamepad = candidate;
            break;
        }
    }

    SDL_free(gamepads);
}

void GamepadInput::closeGamepad()
{
    if (m_gamepad != nullptr)
    {
        SDL_CloseGamepad(m_gamepad);
        m_gamepad = nullptr;
    }
}
