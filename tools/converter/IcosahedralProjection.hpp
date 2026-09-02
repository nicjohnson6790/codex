#pragma once

#include <array>
#include <cstdint>

class IcosahedralProjection
{
public:
    static constexpr double kAuthalicRadiusMeters = 6371007.180918475;
    static constexpr std::uint32_t kVersion = 3;
    static constexpr std::array<double, 3> kOrientationDegrees{-83.65929, 25.44458, -87.45184};
    static constexpr double kAtlasRotationDegrees = -60.0;

    struct Vec2 { double x = 0.0; double y = 0.0; };
    struct Bounds { double minX = 0.0; double minY = 0.0; double maxX = 0.0; double maxY = 0.0; };

    IcosahedralProjection();

    [[nodiscard]] bool inverse(double xMeters, double yMeters, double* longitudeDegrees, double* latitudeDegrees) const;
    [[nodiscard]] Bounds bounds() const { return m_bounds; }
    [[nodiscard]] const char* description() const;

private:
    struct Vec3 { double x = 0.0; double y = 0.0; double z = 0.0; };
    struct Face
    {
        std::array<Vec3, 3> sphere{};
        std::array<Vec2, 3> atlas{};
    };

    std::array<Face, 24> m_faces{};
    Bounds m_bounds{};
};
