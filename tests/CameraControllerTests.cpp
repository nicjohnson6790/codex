#include "AppConfig.hpp"
#include "CityBuilderCameraController.hpp"
#include "FreeFlightCameraController.hpp"

#include <glm/geometric.hpp>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <numbers>

namespace
{
void near(double actual, double expected, double tolerance = 1.0e-9)
{
    assert(std::isfinite(actual));
    assert(std::abs(actual - expected) < tolerance);
}

void validBasis(const CameraManager::Camera& camera)
{
    near(glm::length(camera.forward), 1.0);
    near(glm::length(camera.up), 1.0);
    near(glm::dot(camera.forward, camera.up), 0.0);
    near(glm::cross(camera.forward, camera.up).y, 0.0);
    assert(camera.forward.y <= 1.0e-12 && camera.forward.y >= -1.0);
}

CameraManager::Camera atHeight(double height)
{
    return {.position = Position(0, 0, {100.0, height, 100.0})};
}
}

int main()
{
    CityBuilderCameraController controller;
    GamepadState input{.hasGamepad = true};

    // Pan at the same speed and height at all pitches, including the exact pole.
    for (const double pitch : {0.0, -0.7, -std::numbers::pi / 2.0})
    {
        auto camera = atHeight(100.0);
        camera.forward = {0.0, std::sin(pitch), -std::cos(pitch)};
        camera.up = {0.0, std::cos(pitch), std::sin(pitch)};
        input.leftY = -1.0f;
        controller.update(camera, input, 1.0f);
        near(camera.position.localPosition().y, 100.0);
        near(camera.position.localPosition().z, 100.0 - 28.6);
        validBasis(camera);
    }

    // Fractional triggers, cancellation, descent and altitude-scaled speed.
    input = {.rightTrigger = 0.25f, .hasGamepad = true};
    auto low = atHeight(0.0);
    auto high = atHeight(100.0);
    controller.update(low, input, 1.0f);
    controller.update(high, input, 1.0f);
    near(low.position.localPosition().y, 0.65);
    near(high.position.localPosition().y, 107.15);
    input.leftTrigger = 0.25f;
    controller.update(high, input, 1.0f);
    near(high.position.localPosition().y, 107.15);
    input = {.leftTrigger = 0.5f, .hasGamepad = true};
    low = atHeight(0.0);
    controller.update(low, input, 1.0f);
    near(low.position.localPosition().y, -1.3);

    // Clamp both pitch limits and yaw while looking straight down.
    auto camera = atHeight(0.0);
    input = {.rightY = 1.0f, .hasGamepad = true};
    controller.update(camera, input, 10.0f);
    near(camera.forward.y, -1.0);
    input = {.rightX = 1.0f, .hasGamepad = true};
    controller.update(camera, input, static_cast<float>(std::numbers::pi / (2.0 * AppConfig::Camera::kLookSpeedRadians)));
    validBasis(camera);
    near(camera.up.x, 1.0, 1.0e-6);
    input = {.leftY = -1.0f, .hasGamepad = true};
    const double oldX = camera.position.localPosition().x;
    controller.update(camera, input, 1.0f);
    near(camera.position.localPosition().x, oldX + 2.6, 1.0e-6);
    input = {.rightY = -1.0f, .hasGamepad = true};
    controller.update(camera, input, 10.0f);
    near(camera.forward.y, 0.0);
    validBasis(camera);

    // Entering from rolled/upward free flight preserves position and heading.
    camera = atHeight(300.0);
    camera.forward = glm::normalize(glm::dvec3{1.0, 1.0, -1.0});
    camera.up = glm::normalize(glm::dvec3{1.0, 0.0, 1.0});
    controller.constrain(camera);
    near(camera.position.localPosition().y, 300.0);
    near(camera.forward.y, 0.0);
    near(camera.forward.x, std::sqrt(0.5));
    validBasis(camera);
    const auto beforeDisconnect = camera;
    controller.update(camera, {.leftY = -1.0f, .rightTrigger = 1.0f}, 1.0f);
    near(glm::length(camera.position.offsetTo(beforeDisconnect.position)), 0.0);

    FreeFlightCameraController freeFlight;
    freeFlight.update(camera, {.rightY = -1.0f, .rightShoulder = true, .hasGamepad = true}, 0.2f);
    controller.constrain(camera);
    validBasis(camera);

    // At the downward endpoint the shared view matrix remains finite.
    CameraManager manager;
    manager.createCamera("City Builder", camera.position, {0.0, -1.0, 0.0}, {1.0, 0.0, 0.0});
    const auto matrix = manager.buildActiveViewProjectionMatrix({1280, 800});
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            assert(std::isfinite(matrix[column][row]));

    // Pan across a world-cell boundary without losing the local displacement.
    camera = atHeight(0.0);
    camera.position = Position(23, -42, {Position::kCellSize - 1.0, 0.0, 100.0});
    controller.update(camera, {.leftX = 1.0f, .hasGamepad = true}, 1.0f);
    assert(camera.position.gridX() == 24);
    near(camera.position.localPosition().x, 1.6);
}
