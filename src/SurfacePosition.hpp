#pragma once

#include "Position.hpp"

inline glm::dvec3 surfacePositionRelativeTo(const Position& point, const Position& camera)
{
    const auto cellDelta = [](std::int64_t to, std::int64_t from) {
        return to >= from ? double(std::uint64_t(to) - std::uint64_t(from))
            : -double(std::uint64_t(from) - std::uint64_t(to));
    };
    return point.localPosition() - camera.localPosition() + glm::dvec3(
        cellDelta(point.gridX(), camera.gridX()) * double(Position::kCellSize), 0.0,
        cellDelta(point.gridY(), camera.gridY()) * double(Position::kCellSize));
}
