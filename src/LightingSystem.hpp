#pragma once

#include <glm/vec3.hpp>

class LightingSystem
{
public:
    struct SunLight
    {
        float azimuthDegrees = 40.0f;
        float elevationDegrees = 52.0f;
        glm::vec3 color{ 1.0f, 0.97f, 0.92f };
        float intensity = 1.35f;
    };

    [[nodiscard]] const SunLight& sun() const { return m_sun; }
    [[nodiscard]] SunLight& sun() { return m_sun; }
    [[nodiscard]] glm::vec3 sunDirection() const;

private:
    SunLight m_sun;
};
