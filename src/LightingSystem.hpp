#pragma once

#include "AppConfig.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

class LightingSystem
{
public:
    struct SunLight
    {
        float azimuthDegrees = AppConfig::Light::kSunAzimuthDegrees;
        float elevationDegrees = AppConfig::Light::kSunElevationDegrees;
        float timeOfDayHours = AppConfig::Light::kTimeOfDayHours;
        float dayLengthSeconds = AppConfig::Light::kDayLengthSeconds;
        float timeFactor = AppConfig::Light::kTimeFactor;
        glm::vec3 color{ AppConfig::Light::kSunColor };
        float intensity = AppConfig::Light::kSunIntensity;
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
