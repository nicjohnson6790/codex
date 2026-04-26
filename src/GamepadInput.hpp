#pragma once

#include <SDL3/SDL.h>

#include <string_view>

struct GamepadState
{
    float leftX = 0.0f;
    float leftY = 0.0f;
    float rightX = 0.0f;
    float rightY = 0.0f;
    float leftTrigger = 0.0f;
    float rightTrigger = 0.0f;
    bool leftShoulder = false;
    bool rightShoulder = false;
    bool rightStickPressed = false;
    bool hasGamepad = false;
};

class GamepadInput
{
public:
    GamepadInput() = default;
    ~GamepadInput();

    GamepadInput(const GamepadInput&) = delete;
    GamepadInput& operator=(const GamepadInput&) = delete;

    void initialize();
    void shutdown();
    void handleEvent(const SDL_Event& event);

    [[nodiscard]] GamepadState pollState() const;
    [[nodiscard]] std::string_view gamepadName() const;
    [[nodiscard]] bool hasGamepad() const;

private:
    void openFirstAvailableGamepad();
    void closeGamepad();

    SDL_Gamepad* m_gamepad = nullptr;
};
