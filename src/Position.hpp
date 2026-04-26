#pragma once

#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>

class Position
{
public:
    static constexpr std::int64_t kCellSize = 1LL << 19;

    Position() = default;
    Position(std::int64_t gridX, std::int64_t gridY, const glm::dvec3& localPosition)
        : m_gridX(gridX)
        , m_gridY(gridY)
        , m_localPosition(localPosition)
    {
        normalize();
    }

    [[nodiscard]] std::int64_t gridX() const { return m_gridX; }
    [[nodiscard]] std::int64_t gridY() const { return m_gridY; }
    [[nodiscard]] const glm::dvec3& localPosition() const { return m_localPosition; }

    void setGridX(std::int64_t gridX) { m_gridX = gridX; }
    void setGridY(std::int64_t gridY) { m_gridY = gridY; }
    void setLocalPosition(const glm::dvec3& localPosition)
    {
        m_localPosition = localPosition;
        normalize();
    }

    void setLocalX(double localX)
    {
        m_localPosition.x = localX;
        normalize();
    }

    void setLocalY(double localY)
    {
        m_localPosition.y = localY;
    }

    void setLocalZ(double localZ)
    {
        m_localPosition.z = localZ;
        normalize();
    }

    void addLocalOffset(const glm::dvec3& offset)
    {
        m_localPosition += offset;
        normalize();
    }

    [[nodiscard]] Position translated(const glm::dvec3& offset) const
    {
        Position translatedPosition = *this;
        translatedPosition.addLocalOffset(offset);
        return translatedPosition;
    }

    [[nodiscard]] glm::dvec3 worldPosition() const
    {
        return {
            (static_cast<double>(m_gridX) * static_cast<double>(kCellSize)) + m_localPosition.x,
            m_localPosition.y,
            (static_cast<double>(m_gridY) * static_cast<double>(kCellSize)) + m_localPosition.z,
        };
    }

    [[nodiscard]] glm::dvec3 localCoordinatesInCellOf(const Position& reference) const
    {
        return worldPosition() - reference.worldPosition();
    }

    [[nodiscard]] glm::dvec3 offsetTo(const Position& target) const
    {
        return target.localCoordinatesInCellOf(*this);
    }

private:
    void normalizeAxis(double& localValue, std::int64_t& gridValue)
    {
        const double cellSize = static_cast<double>(kCellSize);
        if (localValue >= 0.0 && localValue < cellSize)
        {
            return;
        }

        const double cellOffset = std::floor(localValue / cellSize);
        gridValue += static_cast<std::int64_t>(cellOffset);
        localValue -= cellOffset * cellSize;

        if (localValue < 0.0)
        {
            localValue += cellSize;
            --gridValue;
        }
        else if (localValue >= cellSize)
        {
            localValue -= cellSize;
            ++gridValue;
        }
    }

    void normalize()
    {
        normalizeAxis(m_localPosition.x, m_gridX);
        normalizeAxis(m_localPosition.z, m_gridY);
    }

    std::int64_t m_gridX = 0;
    std::int64_t m_gridY = 0;
    glm::dvec3 m_localPosition{ 0.0, 0.0, 0.0 };
};
