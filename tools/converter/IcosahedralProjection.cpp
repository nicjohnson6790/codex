#include "IcosahedralProjection.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>

namespace
{
constexpr double kTheta = 0.4636476090008061162; // atan(1/2)
constexpr double kDegreesToRadians = std::numbers::pi / 180.0;
constexpr double kRotateLambda = IcosahedralProjection::kOrientationDegrees[0] * kDegreesToRadians;
constexpr double kRotatePhi = IcosahedralProjection::kOrientationDegrees[1] * kDegreesToRadians;
constexpr double kRotateGamma = IcosahedralProjection::kOrientationDegrees[2] * kDegreesToRadians;

struct Vertex3 { double x; double y; double z; };

Vertex3 normalize(const Vertex3 v)
{
    const double length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return {v.x / length, v.y / length, v.z / length};
}

Vertex3 inverseAiroceanRotation(double longitude, double latitude)
{
    const double cosPhi = std::cos(latitude);
    const double x = std::cos(longitude) * cosPhi;
    const double y = std::sin(longitude) * cosPhi;
    const double z = std::sin(latitude);
    const double k = z * std::cos(kRotateGamma) - y * std::sin(kRotateGamma);
    const double rotatedLongitude = std::atan2(
        y * std::cos(kRotateGamma) + z * std::sin(kRotateGamma),
        x * std::cos(kRotatePhi) + k * std::sin(kRotatePhi));
    const double rotatedLatitude = std::asin(std::clamp(
        k * std::cos(kRotatePhi) - x * std::sin(kRotatePhi), -1.0, 1.0));
    const double geographicLongitude = rotatedLongitude - kRotateLambda;
    const double geographicCosPhi = std::cos(rotatedLatitude);
    return {
        std::cos(geographicLongitude) * geographicCosPhi,
        std::sin(geographicLongitude) * geographicCosPhi,
        std::sin(rotatedLatitude),
    };
}

double cross(const IcosahedralProjection::Vec2 a, const IcosahedralProjection::Vec2 b,
    const IcosahedralProjection::Vec2 c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
}

IcosahedralProjection::IcosahedralProjection()
{
    std::array<Vec3, 14> vertices{};
    const auto setVertex = [&](std::size_t index, double longitude, double latitude)
    {
        const Vertex3 v = inverseAiroceanRotation(longitude, latitude);
        vertices[index] = {v.x, v.y, v.z};
    };
    setVertex(0, 0.0, std::numbers::pi / 2.0);
    setVertex(1, 0.0, -std::numbers::pi / 2.0);
    for (std::size_t i = 0; i < 10; ++i)
    {
        const double longitude = std::fmod(static_cast<double>(i) * 36.0 + 180.0, 360.0) - 180.0;
        setVertex(i + 2, longitude * kDegreesToRadians, (i & 1U) ? kTheta : -kTheta);
    }

    constexpr std::array<std::array<std::size_t, 3>, 20> baseFaces{{
        {{0, 3, 11}}, {{0, 5, 3}}, {{0, 7, 5}}, {{0, 9, 7}}, {{0, 11, 9}},
        {{2, 11, 3}}, {{3, 4, 2}}, {{4, 3, 5}}, {{5, 6, 4}}, {{6, 5, 7}},
        {{7, 8, 6}}, {{8, 7, 9}}, {{9, 10, 8}}, {{10, 9, 11}}, {{11, 2, 10}},
        {{1, 2, 4}}, {{1, 4, 6}}, {{1, 6, 8}}, {{1, 8, 10}}, {{1, 10, 2}},
    }};

    const auto sumNormalized = [&](std::initializer_list<std::size_t> ids)
    {
        Vertex3 sum{0.0, 0.0, 0.0};
        for (const std::size_t id : ids)
        {
            sum.x += vertices[id].x; sum.y += vertices[id].y; sum.z += vertices[id].z;
        }
        return normalize(sum);
    };
    const Vertex3 splitCentroid = sumNormalized({1, 2, 4});
    vertices[12] = {splitCentroid.x, splitCentroid.y, splitCentroid.z};
    const Vertex3 splitMidpoint = sumNormalized({2, 10});
    vertices[13] = {splitMidpoint.x, splitMidpoint.y, splitMidpoint.z};

    std::array<std::array<std::size_t, 3>, 24> faceVertices{};
    std::copy(baseFaces.begin(), baseFaces.end(), faceVertices.begin());
    faceVertices[15] = {{12, 2, 4}};
    faceVertices[20] = {{1, 12, 4}};
    faceVertices[21] = {{1, 2, 12}};
    faceVertices[14] = {{11, 13, 10}};
    faceVertices[22] = {{11, 2, 13}};
    faceVertices[19] = {{1, 13, 2}};
    faceVertices[23] = {{13, 1, 10}};

    // This tree is the Airocean cut pattern. Faces 14, 15, and 19 are split so
    // the interruptions pass around Japan and Australia instead of through them.
    constexpr std::array<int, 24> parents{{
        -1, 0, 1, 11, 13, 6, 7, 1, 7, 8, 9, 10, 11, 12, 13, 6, 8, 10, 17, 21,
        16, 15, 19, 19,
    }};
    constexpr std::array<std::size_t, 24> sourceFace{{
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,15,15,14,19,
    }};

    std::array<std::array<Vec2, 3>, 24> local{};
    for (std::size_t faceIndex = 0; faceIndex < faceVertices.size(); ++faceIndex)
    {
        const auto base = baseFaces[sourceFace[faceIndex]];
        Vertex3 center{0.0, 0.0, 0.0};
        for (const std::size_t id : base)
        {
            center.x += vertices[id].x; center.y += vertices[id].y; center.z += vertices[id].z;
        }
        center = normalize(center);
        Vertex3 axisX = normalize({-center.y, center.x, 0.0});
        if (std::abs(center.z) > 0.999) axisX = {1.0, 0.0, 0.0};
        const Vertex3 axisY{
            center.y * axisX.z - center.z * axisX.y,
            center.z * axisX.x - center.x * axisX.z,
            center.x * axisX.y - center.y * axisX.x,
        };
        for (std::size_t i = 0; i < 3; ++i)
        {
            const Vec3 v = vertices[faceVertices[faceIndex][i]];
            const double denominator = v.x * center.x + v.y * center.y + v.z * center.z;
            local[faceIndex][i] = {
                (v.x * axisX.x + v.y * axisX.y + v.z * axisX.z) / denominator,
                (v.x * axisY.x + v.y * axisY.y + v.z * axisY.z) / denominator,
            };
        }
    }

    const double desiredEdge = kAuthalicRadiusMeters * std::acos(1.0 / std::sqrt(5.0));
    const double rootDx = local[0][1].x - local[0][0].x;
    const double rootDy = local[0][1].y - local[0][0].y;
    const double scale = desiredEdge / std::hypot(rootDx, rootDy);
    for (std::size_t i = 0; i < 3; ++i)
        m_faces[0].atlas[i] = {local[0][i].x * scale, local[0][i].y * scale};

    std::array<bool, 24> placed{};
    placed[0] = true;
    std::function<void(std::size_t)> placeFace = [&](std::size_t faceIndex)
    {
        if (placed[faceIndex]) return;
        const std::size_t parentIndex = static_cast<std::size_t>(parents[faceIndex]);
        placeFace(parentIndex);
        const auto& childIds = faceVertices[faceIndex];
        const auto& parentIds = faceVertices[parentIndex];
        std::array<std::size_t, 2> sharedIds{};
        std::size_t sharedCount = 0, childThird = 0, parentThird = 0;
        for (std::size_t ci = 0; ci < 3; ++ci)
        {
            bool found = false;
            for (const std::size_t parentId : parentIds) if (childIds[ci] == parentId) found = true;
            if (found) sharedIds[sharedCount++] = childIds[ci]; else childThird = ci;
        }
        for (std::size_t pi = 0; pi < 3; ++pi)
            if (parentIds[pi] != sharedIds[0] && parentIds[pi] != sharedIds[1]) parentThird = pi;

        const auto indexOf = [](const auto& ids, const std::size_t id)
        {
            for (std::size_t i = 0; i < 3; ++i) if (ids[i] == id) return i;
            return std::size_t{0};
        };
        const std::size_t ca = indexOf(childIds, sharedIds[0]);
        const std::size_t cb = indexOf(childIds, sharedIds[1]);
        const std::size_t pa = indexOf(parentIds, sharedIds[0]);
        const std::size_t pb = indexOf(parentIds, sharedIds[1]);
        const Vec2 la = local[faceIndex][ca], lb = local[faceIndex][cb], lp = local[faceIndex][childThird];
        const Vec2 a = m_faces[parentIndex].atlas[pa], b = m_faces[parentIndex].atlas[pb];
        const double ldx = lb.x - la.x, ldy = lb.y - la.y;
        const double adx = b.x - a.x, ady = b.y - a.y;
        const double localLengthSquared = ldx * ldx + ldy * ldy;
        const double u = ((lp.x - la.x) * ldx + (lp.y - la.y) * ldy) / localLengthSquared;
        const double v = (ldx * (lp.y - la.y) - ldy * (lp.x - la.x)) / localLengthSquared;
        const Vec2 candidateA{a.x + u * adx - v * ady, a.y + u * ady + v * adx};
        const Vec2 candidateB{a.x + u * adx + v * ady, a.y + u * ady - v * adx};
        const Vec2 parentTip = m_faces[parentIndex].atlas[parentThird];
        const Vec2 childTip = cross(a, b, candidateA) * cross(a, b, parentTip) < 0.0 ? candidateA : candidateB;
        for (std::size_t ci = 0; ci < 3; ++ci)
        {
            if (ci == childThird) m_faces[faceIndex].atlas[ci] = childTip;
            else m_faces[faceIndex].atlas[ci] = m_faces[parentIndex].atlas[indexOf(parentIds, childIds[ci])];
        }
        placed[faceIndex] = true;
    };

    for (std::size_t i = 0; i < m_faces.size(); ++i)
    {
        placeFace(i);
        for (std::size_t j = 0; j < 3; ++j) m_faces[i].sphere[j] = vertices[faceVertices[i][j]];
    }

    constexpr double angle = kAtlasRotationDegrees * kDegreesToRadians;
    m_bounds = {1e100, 1e100, -1e100, -1e100};
    for (Face& face : m_faces)
        for (Vec2& p : face.atlas)
        {
            p = {p.x * std::cos(angle) - p.y * std::sin(angle),
                 p.x * std::sin(angle) + p.y * std::cos(angle)};
            m_bounds.minX = std::min(m_bounds.minX, p.x);
            m_bounds.minY = std::min(m_bounds.minY, p.y);
            m_bounds.maxX = std::max(m_bounds.maxX, p.x);
            m_bounds.maxY = std::max(m_bounds.maxY, p.y);
        }
}

bool IcosahedralProjection::inverse(double xMeters, double yMeters, double* longitudeDegrees, double* latitudeDegrees) const
{
    constexpr double epsilon = 1e-10;
    for (const Face& face : m_faces)
    {
        const Vec2 a = face.atlas[0], b = face.atlas[1], c = face.atlas[2];
        const double denominator = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
        const double w0 = ((b.y - c.y) * (xMeters - c.x) + (c.x - b.x) * (yMeters - c.y)) / denominator;
        const double w1 = ((c.y - a.y) * (xMeters - c.x) + (a.x - c.x) * (yMeters - c.y)) / denominator;
        const double w2 = 1.0 - w0 - w1;
        if (w0 < -epsilon || w1 < -epsilon || w2 < -epsilon) continue;
        Vec3 v{
            w0 * face.sphere[0].x + w1 * face.sphere[1].x + w2 * face.sphere[2].x,
            w0 * face.sphere[0].y + w1 * face.sphere[1].y + w2 * face.sphere[2].y,
            w0 * face.sphere[0].z + w1 * face.sphere[1].z + w2 * face.sphere[2].z,
        };
        const double length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        v.x /= length; v.y /= length; v.z /= length;
        *longitudeDegrees = std::atan2(v.y, v.x) / kDegreesToRadians;
        *latitudeDegrees = std::asin(std::clamp(v.z, -1.0, 1.0)) / kDegreesToRadians;
        return true;
    }
    return false;
}

const char* IcosahedralProjection::description() const
{
    return "Airocean one-island icosahedral gnomonic net v3";
}
