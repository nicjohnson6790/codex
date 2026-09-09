#pragma once
#include <algorithm>
#include <cmath>
#include "PeriodicWorldPhase.hpp"

// Rows are the front normal and its stretched perpendicular. Round once to the
// uploaded float coefficients before evaluating any universe-scale origin phase.
inline glm::mat2 cloudMacroTransform(float cyclesPerMeter, float angle, float stretch)
{
    const float c=std::cos(angle), s=std::sin(angle);
    return {c*cyclesPerMeter,-s*(cyclesPerMeter/stretch),
            s*cyclesPerMeter,c*(cyclesPerMeter/stretch)};
}

inline void advanceCloudMacroPhases(double& travel, double& evolution,
    float speed, float cyclesPerMeter, float evolutionSpeed, double dt)
{
    travel=WorldPhase::fract(travel+double(speed)*double(cyclesPerMeter)*dt);
    evolution=WorldPhase::fract(evolution+double(evolutionSpeed)*dt/3600.0);
}

inline int waterCloudSampleCount(int primary, float multiplier)
{
    primary=std::max(primary,1);
    return std::clamp(int(std::round(primary*std::clamp(multiplier,0.1f,1.0f))),1,primary);
}
