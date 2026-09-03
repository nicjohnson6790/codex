#pragma once

#include "Position.hpp"

#include <glm/mat2x2.hpp>
#include <glm/vec2.hpp>

#include <cmath>
#include <cstdint>

namespace WorldPhase
{
[[nodiscard]] inline double fract(double value)
{
    return value - std::floor(value);
}

// Computes fract(multiplier * value) without converting the full signed
// multiplier to floating point. Intermediate values always remain one cycle.
[[nodiscard]] inline double multiplyIntegerModuloOne(std::int64_t multiplier, double value)
{
    double contribution = fract(value);
    double result = 0.0;
    const bool negative = multiplier < 0;
    std::uint64_t magnitude = negative
        ? static_cast<std::uint64_t>(-(multiplier + 1)) + 1u
        : static_cast<std::uint64_t>(multiplier);

    while (magnitude != 0u)
    {
        if ((magnitude & 1u) != 0u)
        {
            result = fract(result + contribution);
        }
        contribution = fract(contribution + contribution);
        magnitude >>= 1u;
    }

    return negative && result != 0.0 ? 1.0 - result : result;
}

[[nodiscard]] inline double periodicLinearPhase(const Position& position, const glm::dvec2& cyclesPerMeter)
{
    const double gridXContribution = multiplyIntegerModuloOne(
        position.gridX(), fract(static_cast<double>(Position::kCellSize) * cyclesPerMeter.x));
    const double gridZContribution = multiplyIntegerModuloOne(
        position.gridY(), fract(static_cast<double>(Position::kCellSize) * cyclesPerMeter.y));
    const glm::dvec3& local = position.localPosition();
    return fract(gridXContribution + gridZContribution +
                 fract((local.x * cyclesPerMeter.x) + (local.z * cyclesPerMeter.y)));
}

[[nodiscard]] inline glm::dvec2 periodicWorldPhase(const Position& position, const glm::dmat2& worldToUv)
{
    // GLM matrices are column-major: these are the two rows used by
    // worldToUv * vec2(worldX, worldZ), matching GLSL multiplication.
    return {
        periodicLinearPhase(position, {worldToUv[0][0], worldToUv[1][0]}),
        periodicLinearPhase(position, {worldToUv[0][1], worldToUv[1][1]}),
    };
}

[[nodiscard]] inline glm::dvec2 periodicWorldPhase(const Position& position, double cyclesPerMeter)
{
    return periodicWorldPhase(position, glm::dmat2(cyclesPerMeter, 0.0, 0.0, cyclesPerMeter));
}
} // namespace WorldPhase
