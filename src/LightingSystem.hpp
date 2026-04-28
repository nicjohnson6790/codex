#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

class LightingSystem
{
public:
    struct SunLight
    {
        float azimuthDegrees = 40.0f;
        float elevationDegrees = 52.0f;
        float timeOfDayHours = 10.0f;
        float dayLengthSeconds = 120.0f;
        float timeFactor = 1.0f;
        glm::vec3 color{ 1.0f, 0.97f, 0.92f };
        float intensity = 1.35f;
    };

    [[nodiscard]] const SunLight& sun() const { return m_sun; }
    [[nodiscard]] SunLight& sun() { return m_sun; }
    void advanceTime(float deltaTimeSeconds);
    [[nodiscard]] glm::vec3 sunOrbitAxis() const;
    [[nodiscard]] glm::vec3 sunDirection() const;
    [[nodiscard]] glm::mat4 skyboxRotationMatrix() const;

private:
    [[nodiscard]] static glm::vec3 directionFromAngles(float azimuthDegrees, float elevationDegrees);
    [[nodiscard]] static glm::vec3 orbitReferenceVector(const glm::vec3& axis);

    SunLight m_sun;
};
