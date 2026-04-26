#include "LightingSystem.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <cmath>

glm::vec3 LightingSystem::sunDirection() const
{
    const float azimuthRadians = glm::radians(m_sun.azimuthDegrees);
    const float elevationRadians = glm::radians(glm::clamp(m_sun.elevationDegrees, -89.0f, 89.0f));

    const float cosElevation = std::cos(elevationRadians);
    return glm::normalize(glm::vec3(
        std::cos(azimuthRadians) * cosElevation,
        std::sin(elevationRadians),
        std::sin(azimuthRadians) * cosElevation
    ));
}
