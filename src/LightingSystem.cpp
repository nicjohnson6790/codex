#include "LightingSystem.hpp"

#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

void LightingSystem::advanceTime(float deltaTimeSeconds)
{
    const float clampedDayLength = std::max(m_sun.dayLengthSeconds, 0.001f);
    const float dayProgressDelta = (deltaTimeSeconds * m_sun.timeFactor) / clampedDayLength;
    m_sun.timeOfDayHours = std::fmod(m_sun.timeOfDayHours + (dayProgressDelta * 24.0f), 24.0f);
    if (m_sun.timeOfDayHours < 0.0f)
    {
        m_sun.timeOfDayHours += 24.0f;
    }
}

glm::vec3 LightingSystem::sunOrbitAxis() const
{
    return directionFromAngles(m_sun.azimuthDegrees, m_sun.elevationDegrees);
}

glm::vec3 LightingSystem::sunDirection() const
{
    const glm::vec3 axis = sunOrbitAxis();
    const glm::vec3 reference = orbitReferenceVector(axis);
    const float orbitRadians = glm::radians((m_sun.timeOfDayHours / 24.0f) * 360.0f);
    const glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), orbitRadians, axis);
    return glm::normalize(glm::vec3(rotation * glm::vec4(reference, 0.0f)));
}

glm::mat4 LightingSystem::skyboxRotationMatrix() const
{
    const float orbitRadians = glm::radians((m_sun.timeOfDayHours / 24.0f) * 360.0f);
    return glm::rotate(glm::mat4(1.0f), orbitRadians, sunOrbitAxis());
}

glm::vec3 LightingSystem::directionFromAngles(float azimuthDegrees, float elevationDegrees)
{
    const float azimuthRadians = glm::radians(azimuthDegrees);
    const float elevationRadians = glm::radians(glm::clamp(elevationDegrees, -89.0f, 89.0f));

    const float cosElevation = std::cos(elevationRadians);
    return glm::normalize(glm::vec3(
        std::cos(azimuthRadians) * cosElevation,
        std::sin(elevationRadians),
        std::sin(azimuthRadians) * cosElevation
    ));
}

glm::vec3 LightingSystem::orbitReferenceVector(const glm::vec3& axis)
{
    const glm::vec3 upReference(0.0f, 1.0f, 0.0f);
    const glm::vec3 fallbackReference(1.0f, 0.0f, 0.0f);
    const glm::vec3 baseReference = std::abs(glm::dot(axis, upReference)) > 0.98f
        ? fallbackReference
        : upReference;
    return glm::normalize(glm::cross(axis, baseReference));
}
