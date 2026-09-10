#include "PeriodicWorldPhase.hpp"
#include "SurfacePosition.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace
{
double wrappedDelta(double a, double b)
{
    double delta = WorldPhase::fract(a - b);
    return delta > 0.5 ? delta - 1.0 : delta;
}

void expectNear(double actual, double expected, double tolerance = 1.0e-10)
{
    assert(std::abs(wrappedDelta(actual, expected)) <= tolerance);
}

void expectPhaseNear(const glm::dvec2& actual, const glm::dvec2& expected, double tolerance = 1.0e-10)
{
    expectNear(actual.x, expected.x, tolerance);
    expectNear(actual.y, expected.y, tolerance);
    assert(actual.x >= 0.0 && actual.x < 1.0);
    assert(actual.y >= 0.0 && actual.y < 1.0);
}

glm::dvec2 modestReference(const Position& position, const glm::dmat2& transform)
{
    const glm::dvec3 world = position.worldPosition();
    const glm::dvec2 uv = transform * glm::dvec2(world.x, world.z);
    return {WorldPhase::fract(uv.x), WorldPhase::fract(uv.y)};
}
} // namespace

int main()
{
    const Position positive(12, 7, {123.25, 4.0, 456.75});
    const Position negative(-9, -4, {321.5, 0.0, 17.25});
    const glm::dmat2 axisAligned(1.0 / 14.0, 0.0, 0.0, 1.0 / 8.0);
    expectPhaseNear(WorldPhase::periodicWorldPhase(positive, axisAligned), modestReference(positive, axisAligned));
    expectPhaseNear(WorldPhase::periodicWorldPhase(negative, axisAligned), modestReference(negative, axisAligned));

    // Non-axis-aligned, non-uniform transform. Explicit expected equations
    // make this sensitive to matrix transposition and transform order.
    const double angle = 0.37;
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const glm::dmat2 rotated(c * 0.13, -s * 0.13, s * 0.07, c * 0.07);
    const Position transformPosition(3, -2, {71.25, 0.0, 19.5});
    const glm::dvec3 transformWorld = transformPosition.worldPosition();
    const glm::dvec2 manualExpected{
        WorldPhase::fract((c * 0.13 * transformWorld.x) + (s * 0.07 * transformWorld.z)),
        WorldPhase::fract((-s * 0.13 * transformWorld.x) + (c * 0.07 * transformWorld.z)),
    };
    expectPhaseNear(WorldPhase::periodicWorldPhase(transformPosition, rotated), manualExpected, 2.0e-10);

    // Crossing normalized cell borders changes phase only by physical motion.
    constexpr double epsilon = 0.125;
    const glm::dmat2 boundaryTransform(0.017, -0.011, 0.023, 0.031);
    const Position beforeX(41, -17, {static_cast<double>(Position::kCellSize) - epsilon, 0.0, 200.0});
    const Position afterX(42, -17, {epsilon, 0.0, 200.0});
    const glm::dvec2 beforeXPhase = WorldPhase::periodicWorldPhase(beforeX, boundaryTransform);
    const glm::dvec2 afterXPhase = WorldPhase::periodicWorldPhase(afterX, boundaryTransform);
    expectNear(afterXPhase.x, beforeXPhase.x + (2.0 * epsilon * boundaryTransform[0][0]));
    expectNear(afterXPhase.y, beforeXPhase.y + (2.0 * epsilon * boundaryTransform[0][1]));

    const Position beforeZ(-1, -1, {90.0, 0.0, static_cast<double>(Position::kCellSize) - epsilon});
    const Position afterZ(-1, 0, {90.0, 0.0, epsilon});
    const glm::dvec2 beforeZPhase = WorldPhase::periodicWorldPhase(beforeZ, boundaryTransform);
    const glm::dvec2 afterZPhase = WorldPhase::periodicWorldPhase(afterZ, boundaryTransform);
    expectNear(afterZPhase.x, beforeZPhase.x + (2.0 * epsilon * boundaryTransform[1][0]));
    expectNear(afterZPhase.y, beforeZPhase.y + (2.0 * epsilon * boundaryTransform[1][1]));

    // Tiny local movement remains observable at universe-scale grid values.
    constexpr std::array<std::int64_t, 4> hugeGrids{
        std::int64_t{1} << 32, std::int64_t{1} << 40, std::int64_t{1} << 50, std::int64_t{1} << 60};
    constexpr std::array<std::int64_t, 2> signs{1, -1};
    for (const std::int64_t grid : hugeGrids)
    {
        for (const std::int64_t sign : signs)
        {
            const Position huge(sign * grid, -sign * grid, {11.0, 0.0, 23.0});
            const Position moved(sign * grid, -sign * grid, {11.03125, 0.0, 22.984375});
            const auto relative = surfacePositionRelativeTo(moved, huge);
            expectNear(relative.x, 0.03125);
            expectNear(relative.z, -0.015625);
            const Position beforeWrap(sign * grid, -sign * grid, {524287.75, 4.0, 0.25});
            const Position afterWrap(sign * grid + 1, -sign * grid - 1, {0.25, 7.0, 524287.75});
            const auto wrappedRelative = surfacePositionRelativeTo(afterWrap, beforeWrap);
            expectNear(wrappedRelative.x, 0.5);
            expectNear(wrappedRelative.y, 3.0);
            expectNear(wrappedRelative.z, -0.5);
            const glm::dvec2 phase = WorldPhase::periodicWorldPhase(huge, boundaryTransform);
            const glm::dvec2 movedPhase = WorldPhase::periodicWorldPhase(moved, boundaryTransform);
            expectNear(movedPhase.x, phase.x + (0.03125 * boundaryTransform[0][0]) - (0.015625 * boundaryTransform[1][0]));
            expectNear(movedPhase.y, phase.y + (0.03125 * boundaryTransform[0][1]) - (0.015625 * boundaryTransform[1][1]));
        }
    }

    assert(WorldPhase::multiplyIntegerModuloOne(0, 0.37) == 0.0);
    expectNear(WorldPhase::multiplyIntegerModuloOne(std::numeric_limits<std::int64_t>::max(), 0.25), 0.75);
    expectNear(WorldPhase::multiplyIntegerModuloOne(-std::numeric_limits<std::int64_t>::max(), 0.25), 0.25);
    expectNear(WorldPhase::multiplyIntegerModuloOne(std::numeric_limits<std::int64_t>::min(), 0.25), 0.0);

    const Position signedMinimum(std::numeric_limits<std::int64_t>::min(),
                                 std::numeric_limits<std::int64_t>::max(), {0.5, 0.0, 0.25});
    const glm::dvec2 minimumPhase = WorldPhase::periodicWorldPhase(signedMinimum, rotated);
    assert(minimumPhase.x >= 0.0 && minimumPhase.x < 1.0);
    assert(minimumPhase.y >= 0.0 && minimumPhase.y < 1.0);
}
