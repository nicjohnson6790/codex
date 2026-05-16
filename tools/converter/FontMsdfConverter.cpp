#include "FontMsdfConverter.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{

constexpr std::uint32_t kAtlasSize = 1024u;
constexpr std::uint32_t kFirstCodepoint = 32u;
constexpr std::uint32_t kLastCodepoint = 126u;
constexpr std::uint32_t kPixelSize = 64u;
constexpr float kDistanceRangePixels = 8.0f;
constexpr float kPaddingPixels = kDistanceRangePixels + 2.0f;
constexpr float kPi = 3.14159265358979323846f;

struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;
};

struct Segment
{
    enum class Type
    {
        Line,
        Quadratic,
        Cubic,
    };

    Type type = Type::Line;
    Vec2 a;
    Vec2 control0;
    Vec2 control1;
    Vec2 b;
    std::uint32_t colorMask = 0x7u;
};

struct GlyphShape
{
    std::vector<Segment> segments;
    std::vector<std::vector<std::size_t>> contourSegmentIndices;
};

struct SignedDistance
{
    float distance = -std::numeric_limits<float>::max();
    float dot = 0.0f;
};

bool operator<(SignedDistance lhs, SignedDistance rhs)
{
    return std::abs(lhs.distance) < std::abs(rhs.distance) ||
        (std::abs(lhs.distance) == std::abs(rhs.distance) && lhs.dot < rhs.dot);
}

struct OutlineBuildContext
{
    GlyphShape shape;
    Vec2 current{};
    Vec2 contourStart{};
    std::vector<std::size_t> currentContourSegments;
};

struct EdgeDistance
{
    SignedDistance trueDistance{};
    float nearParam = 0.0f;
    const Segment* nearEdge = nullptr;
    float minNegativePerpendicularDistance = -std::numeric_limits<float>::max();
    float minPositivePerpendicularDistance = std::numeric_limits<float>::max();
};

float FromFt26Dot6(FT_Pos value)
{
    return static_cast<float>(value) / 64.0f;
}

Vec2 FromFtVector(const FT_Vector* value)
{
    return Vec2{ FromFt26Dot6(value->x), FromFt26Dot6(value->y) };
}

Vec2 Lerp(Vec2 a, Vec2 b, float t)
{
    return Vec2{
        a.x + ((b.x - a.x) * t),
        a.y + ((b.y - a.y) * t),
    };
}

Vec2 Quadratic(Vec2 a, Vec2 b, Vec2 c, float t)
{
    return Lerp(Lerp(a, b, t), Lerp(b, c, t), t);
}

Vec2 Cubic(Vec2 a, Vec2 b, Vec2 c, Vec2 d, float t)
{
    return Lerp(Quadratic(a, b, c, t), Quadratic(b, c, d, t), t);
}

Vec2 SegmentPoint(const Segment& segment, float t)
{
    switch (segment.type)
    {
    case Segment::Type::Line:
        return Lerp(segment.a, segment.b, t);
    case Segment::Type::Quadratic:
        return Quadratic(segment.a, segment.control0, segment.b, t);
    case Segment::Type::Cubic:
        return Cubic(segment.a, segment.control0, segment.control1, segment.b, t);
    }
    return segment.a;
}

Vec2 SegmentDirection(const Segment& segment, float t)
{
    switch (segment.type)
    {
    case Segment::Type::Line:
        return {
            segment.b.x - segment.a.x,
            segment.b.y - segment.a.y,
        };
    case Segment::Type::Quadratic:
        {
            const Vec2 tangent = Lerp(
                { segment.control0.x - segment.a.x, segment.control0.y - segment.a.y },
                { segment.b.x - segment.control0.x, segment.b.y - segment.control0.y },
                t);
            if (tangent.x == 0.0f && tangent.y == 0.0f)
            {
                return { segment.b.x - segment.a.x, segment.b.y - segment.a.y };
            }
            return tangent;
        }
    case Segment::Type::Cubic:
        {
            const Vec2 tangent = Lerp(
                Lerp(
                    { segment.control0.x - segment.a.x, segment.control0.y - segment.a.y },
                    { segment.control1.x - segment.control0.x, segment.control1.y - segment.control0.y },
                    t),
                Lerp(
                    { segment.control1.x - segment.control0.x, segment.control1.y - segment.control0.y },
                    { segment.b.x - segment.control1.x, segment.b.y - segment.control1.y },
                    t),
                t);
            if (tangent.x == 0.0f && tangent.y == 0.0f)
            {
                if (t == 0.0f)
                {
                    return { segment.control1.x - segment.a.x, segment.control1.y - segment.a.y };
                }
                if (t == 1.0f)
                {
                    return { segment.b.x - segment.control0.x, segment.b.y - segment.control0.y };
                }
            }
            return tangent;
        }
    }
    return { 1.0f, 0.0f };
}

Vec2 SegmentDirectionMsdf(const Segment& segment, float t)
{
    switch (segment.type)
    {
    case Segment::Type::Line:
        return {
            segment.b.x - segment.a.x,
            segment.b.y - segment.a.y,
        };
    case Segment::Type::Quadratic:
        return Lerp(
            { segment.control0.x - segment.a.x, segment.control0.y - segment.a.y },
            { segment.b.x - segment.control0.x, segment.b.y - segment.control0.y },
            t);
    case Segment::Type::Cubic:
        {
            const Vec2 first = Lerp(
                { segment.control0.x - segment.a.x, segment.control0.y - segment.a.y },
                { segment.control1.x - segment.control0.x, segment.control1.y - segment.control0.y },
                t);
            const Vec2 second = Lerp(
                { segment.control1.x - segment.control0.x, segment.control1.y - segment.control0.y },
                { segment.b.x - segment.control1.x, segment.b.y - segment.control1.y },
                t);
            return Lerp(first, second, t);
        }
    }
    return { 1.0f, 0.0f };
}

float Cross(Vec2 a, Vec2 b);
int Sign(float value);
float Median(float a, float b, float c);

void AddSegment(OutlineBuildContext* context, Segment segment)
{
    const float dx = segment.b.x - segment.a.x;
    const float dy = segment.b.y - segment.a.y;
    if ((dx * dx) + (dy * dy) < 0.0001f)
    {
        return;
    }

    context->shape.segments.push_back(segment);
    context->currentContourSegments.push_back(context->shape.segments.size() - 1u);
}

void AddLineSegment(OutlineBuildContext* context, Vec2 a, Vec2 b)
{
    Segment segment{};
    segment.type = Segment::Type::Line;
    segment.a = a;
    segment.b = b;
    AddSegment(context, segment);
}

void AddQuadraticSegment(OutlineBuildContext* context, Vec2 a, Vec2 control, Vec2 b)
{
    if (std::abs(Cross({ control.x - a.x, control.y - a.y }, { b.x - control.x, b.y - control.y })) <= 0.000001f)
    {
        AddLineSegment(context, a, b);
        return;
    }

    Segment segment{};
    segment.type = Segment::Type::Quadratic;
    segment.a = a;
    segment.control0 = control;
    segment.b = b;
    AddSegment(context, segment);
}

void AddCubicSegment(OutlineBuildContext* context, Vec2 a, Vec2 control0, Vec2 control1, Vec2 b)
{
    const Vec2 p12{ control1.x - control0.x, control1.y - control0.y };
    if (std::abs(Cross({ control0.x - a.x, control0.y - a.y }, p12)) <= 0.000001f &&
        std::abs(Cross(p12, { b.x - control1.x, b.y - control1.y })) <= 0.000001f)
    {
        AddLineSegment(context, a, b);
        return;
    }
    const Vec2 quadraticControl0{ (1.5f * control0.x) - (0.5f * a.x), (1.5f * control0.y) - (0.5f * a.y) };
    const Vec2 quadraticControl1{ (1.5f * control1.x) - (0.5f * b.x), (1.5f * control1.y) - (0.5f * b.y) };
    if (std::abs(quadraticControl0.x - quadraticControl1.x) <= 0.000001f &&
        std::abs(quadraticControl0.y - quadraticControl1.y) <= 0.000001f)
    {
        AddQuadraticSegment(context, a, quadraticControl0, b);
        return;
    }

    Segment segment{};
    segment.type = Segment::Type::Cubic;
    segment.a = a;
    segment.control0 = control0;
    segment.control1 = control1;
    segment.b = b;
    AddSegment(context, segment);
}

void FinishContour(OutlineBuildContext* context)
{
    if (context->currentContourSegments.empty())
    {
        return;
    }

    AddLineSegment(context, context->current, context->contourStart);
    if (!context->currentContourSegments.empty())
    {
        context->shape.contourSegmentIndices.push_back(std::move(context->currentContourSegments));
        context->currentContourSegments.clear();
    }
}

int MoveToCallback(const FT_Vector* to, void* user)
{
    auto* context = static_cast<OutlineBuildContext*>(user);
    FinishContour(context);
    context->current = FromFtVector(to);
    context->contourStart = context->current;
    return 0;
}

int LineToCallback(const FT_Vector* to, void* user)
{
    auto* context = static_cast<OutlineBuildContext*>(user);
    const Vec2 destination = FromFtVector(to);
    AddLineSegment(context, context->current, destination);
    context->current = destination;
    return 0;
}

int ConicToCallback(const FT_Vector* control, const FT_Vector* to, void* user)
{
    auto* context = static_cast<OutlineBuildContext*>(user);
    const Vec2 controlPoint = FromFtVector(control);
    const Vec2 destination = FromFtVector(to);
    AddQuadraticSegment(context, context->current, controlPoint, destination);
    context->current = destination;
    return 0;
}

int CubicToCallback(const FT_Vector* control1, const FT_Vector* control2, const FT_Vector* to, void* user)
{
    auto* context = static_cast<OutlineBuildContext*>(user);
    const Vec2 controlPoint1 = FromFtVector(control1);
    const Vec2 controlPoint2 = FromFtVector(control2);
    const Vec2 destination = FromFtVector(to);
    AddCubicSegment(context, context->current, controlPoint1, controlPoint2, destination);
    context->current = destination;
    return 0;
}

void NormalizeShape(GlyphShape* shape);
void ColorShapeEdges(GlyphShape* shape);
bool PointInShape(Vec2 point, const GlyphShape& shape);

bool BuildShape(const FT_Outline& outline, GlyphShape* outShape, std::string* error)
{
    OutlineBuildContext context{};
    FT_Outline_Funcs callbacks{};
    callbacks.move_to = MoveToCallback;
    callbacks.line_to = LineToCallback;
    callbacks.conic_to = ConicToCallback;
    callbacks.cubic_to = CubicToCallback;
    callbacks.shift = 0;
    callbacks.delta = 0;

    const FT_Error result = FT_Outline_Decompose(&const_cast<FT_Outline&>(outline), &callbacks, &context);
    if (result != 0)
    {
        *error = "FT_Outline_Decompose failed";
        return false;
    }
    FinishContour(&context);
    NormalizeShape(&context.shape);
    ColorShapeEdges(&context.shape);

    *outShape = std::move(context.shape);
    return true;
}

float Dot(Vec2 a, Vec2 b)
{
    return (a.x * b.x) + (a.y * b.y);
}

float Cross(Vec2 a, Vec2 b)
{
    return (a.x * b.y) - (a.y * b.x);
}

float NonZeroSign(float value)
{
    return value < 0.0f ? -1.0f : 1.0f;
}

float Length(Vec2 value)
{
    return std::sqrt((value.x * value.x) + (value.y * value.y));
}

Vec2 Normalize(Vec2 value, bool allowZero = false)
{
    const float length = Length(value);
    if (length <= 0.000001f)
    {
        return { 0.0f, allowZero ? 0.0f : 1.0f };
    }
    return { value.x / length, value.y / length };
}

int SolveQuadratic(float* roots, float a, float b, float c)
{
    if (std::abs(a) < 0.000001f)
    {
        if (std::abs(b) < 0.000001f)
        {
            return 0;
        }
        roots[0] = -c / b;
        return 1;
    }

    const float discriminant = (b * b) - (4.0f * a * c);
    if (discriminant < 0.0f)
    {
        return 0;
    }
    if (discriminant < 0.000001f)
    {
        roots[0] = -b / (2.0f * a);
        return 1;
    }

    const float root = std::sqrt(discriminant);
    roots[0] = (-b - root) / (2.0f * a);
    roots[1] = (-b + root) / (2.0f * a);
    if (roots[0] > roots[1])
    {
        std::swap(roots[0], roots[1]);
    }
    return 2;
}

int SolveCubic(float* roots, float a, float b, float c, float d)
{
    if (std::abs(a) < 0.000001f)
    {
        return SolveQuadratic(roots, b, c, d);
    }

    const float invA = 1.0f / a;
    const float bb = b * invA;
    const float cc = c * invA;
    const float dd = d * invA;
    const float p = cc - ((bb * bb) / 3.0f);
    const float q = ((2.0f * bb * bb * bb) / 27.0f) - ((bb * cc) / 3.0f) + dd;
    const float offset = bb / 3.0f;
    const float discriminant = ((q * q) / 4.0f) + ((p * p * p) / 27.0f);

    if (discriminant > 0.000001f)
    {
        const float root = std::sqrt(discriminant);
        roots[0] = std::cbrt((-q * 0.5f) + root) + std::cbrt((-q * 0.5f) - root) - offset;
        return 1;
    }
    if (std::abs(discriminant) <= 0.000001f)
    {
        const float u = std::cbrt(-q * 0.5f);
        roots[0] = (2.0f * u) - offset;
        roots[1] = -u - offset;
        if (roots[0] > roots[1])
        {
            std::swap(roots[0], roots[1]);
        }
        return std::abs(roots[0] - roots[1]) < 0.000001f ? 1 : 2;
    }

    const float radius = 2.0f * std::sqrt(-p / 3.0f);
    const float argument = std::clamp((3.0f * q / (2.0f * p)) * std::sqrt(-3.0f / p), -1.0f, 1.0f);
    const float angle = std::acos(argument) / 3.0f;
    roots[0] = (radius * std::cos(angle)) - offset;
    roots[1] = (radius * std::cos(angle - (2.0f * kPi / 3.0f))) - offset;
    roots[2] = (radius * std::cos(angle - (4.0f * kPi / 3.0f))) - offset;
    std::sort(roots, roots + 3);
    return 3;
}

SignedDistance SignedDistanceToSegment(Vec2 point, const Segment& segment, float* outParam = nullptr)
{
    float param = 0.0f;
    SignedDistance result{};

    switch (segment.type)
    {
    case Segment::Type::Line:
        {
            const Vec2 aq{
                point.x - segment.a.x,
                point.y - segment.a.y,
            };
            const Vec2 ab{
                segment.b.x - segment.a.x,
                segment.b.y - segment.a.y,
            };
            const float abLengthSquared = Dot(ab, ab);
            param = abLengthSquared > 0.0f ? Dot(aq, ab) / abLengthSquared : 0.0f;
            const Vec2 endpoint = param > 0.5f ? segment.b : segment.a;
            const Vec2 eq{
                endpoint.x - point.x,
                endpoint.y - point.y,
            };
            const float endpointDistance = Length(eq);
            if (param > 0.0f && param < 1.0f)
            {
                const Vec2 orthonormal = Normalize({ ab.y, -ab.x });
                const float orthogonalDistance = Dot(orthonormal, aq);
                if (std::abs(orthogonalDistance) < endpointDistance)
                {
                    result = SignedDistance{ orthogonalDistance, 0.0f };
                    break;
                }
            }
            result = SignedDistance{
                NonZeroSign(Cross(aq, ab)) * endpointDistance,
                std::abs(Dot(Normalize(ab), Normalize(eq))),
            };
            break;
        }
    case Segment::Type::Quadratic:
        {
            const Vec2 qa{
                segment.a.x - point.x,
                segment.a.y - point.y,
            };
            const Vec2 ab{
                segment.control0.x - segment.a.x,
                segment.control0.y - segment.a.y,
            };
            const Vec2 br{
                segment.b.x - segment.control0.x - ab.x,
                segment.b.y - segment.control0.y - ab.y,
            };
            float roots[3]{};
            const int rootCount = SolveCubic(
                roots,
                Dot(br, br),
                3.0f * Dot(ab, br),
                (2.0f * Dot(ab, ab)) + Dot(qa, br),
                Dot(qa, ab));

            Vec2 endpointDirection = SegmentDirectionMsdf(segment, 0.0f);
            float minDistance = NonZeroSign(Cross(endpointDirection, qa)) * Length(qa);
            param = -Dot(qa, endpointDirection) / std::max(Dot(endpointDirection, endpointDirection), 0.000001f);

            const Vec2 bq{
                segment.b.x - point.x,
                segment.b.y - point.y,
            };
            const float bDistance = Length(bq);
            if (bDistance < std::abs(minDistance))
            {
                endpointDirection = SegmentDirectionMsdf(segment, 1.0f);
                minDistance = NonZeroSign(Cross(endpointDirection, bq)) * bDistance;
                param = Dot({ point.x - segment.control0.x, point.y - segment.control0.y }, endpointDirection) /
                    std::max(Dot(endpointDirection, endpointDirection), 0.000001f);
            }

            for (int index = 0; index < rootCount; ++index)
            {
                const float t = roots[index];
                if (t > 0.0f && t < 1.0f)
                {
                    const Vec2 qe{
                        qa.x + (2.0f * t * ab.x) + (t * t * br.x),
                        qa.y + (2.0f * t * ab.y) + (t * t * br.y),
                    };
                    const float distance = Length(qe);
                    if (distance <= std::abs(minDistance))
                    {
                        minDistance = NonZeroSign(Cross({ ab.x + (t * br.x), ab.y + (t * br.y) }, qe)) * distance;
                        param = t;
                    }
                }
            }

            if (param >= 0.0f && param <= 1.0f)
            {
                result = SignedDistance{ minDistance, 0.0f };
            }
            else if (param < 0.5f)
            {
                result = SignedDistance{ minDistance, std::abs(Dot(Normalize(SegmentDirectionMsdf(segment, 0.0f)), Normalize(qa))) };
            }
            else
            {
                result = SignedDistance{ minDistance, std::abs(Dot(Normalize(SegmentDirectionMsdf(segment, 1.0f)), Normalize(bq))) };
            }
            break;
        }
    case Segment::Type::Cubic:
        {
            const Vec2 qa{
                segment.a.x - point.x,
                segment.a.y - point.y,
            };
            const Vec2 ab{
                segment.control0.x - segment.a.x,
                segment.control0.y - segment.a.y,
            };
            const Vec2 br{
                segment.control1.x - segment.control0.x - ab.x,
                segment.control1.y - segment.control0.y - ab.y,
            };
            const Vec2 as{
                segment.b.x - (3.0f * segment.control1.x) + (3.0f * segment.control0.x) - segment.a.x,
                segment.b.y - (3.0f * segment.control1.y) + (3.0f * segment.control0.y) - segment.a.y,
            };

            Vec2 endpointDirection = SegmentDirectionMsdf(segment, 0.0f);
            float minDistance = NonZeroSign(Cross(endpointDirection, qa)) * Length(qa);
            param = -Dot(qa, endpointDirection) / std::max(Dot(endpointDirection, endpointDirection), 0.000001f);

            const Vec2 bq{
                segment.b.x - point.x,
                segment.b.y - point.y,
            };
            const float bDistance = Length(bq);
            if (bDistance < std::abs(minDistance))
            {
                endpointDirection = SegmentDirectionMsdf(segment, 1.0f);
                minDistance = NonZeroSign(Cross(endpointDirection, bq)) * bDistance;
                param = Dot({ endpointDirection.x - bq.x, endpointDirection.y - bq.y }, endpointDirection) /
                    std::max(Dot(endpointDirection, endpointDirection), 0.000001f);
            }

            constexpr int kSearchStarts = 4;
            constexpr int kSearchSteps = 4;
            for (int start = 0; start <= kSearchStarts; ++start)
            {
                float t = static_cast<float>(start) / static_cast<float>(kSearchStarts);
                Vec2 qe{
                    qa.x + (3.0f * t * ab.x) + (3.0f * t * t * br.x) + (t * t * t * as.x),
                    qa.y + (3.0f * t * ab.y) + (3.0f * t * t * br.y) + (t * t * t * as.y),
                };
                Vec2 d1{
                    (3.0f * ab.x) + (6.0f * t * br.x) + (3.0f * t * t * as.x),
                    (3.0f * ab.y) + (6.0f * t * br.y) + (3.0f * t * t * as.y),
                };
                Vec2 d2{
                    (6.0f * br.x) + (6.0f * t * as.x),
                    (6.0f * br.y) + (6.0f * t * as.y),
                };
                float denominator = Dot(d1, d1) + Dot(qe, d2);
                if (std::abs(denominator) <= 0.000001f)
                {
                    continue;
                }
                float improvedT = t - (Dot(qe, d1) / denominator);
                if (improvedT > 0.0f && improvedT < 1.0f)
                {
                    int remainingSteps = kSearchSteps;
                    do
                    {
                        t = improvedT;
                        qe = {
                            qa.x + (3.0f * t * ab.x) + (3.0f * t * t * br.x) + (t * t * t * as.x),
                            qa.y + (3.0f * t * ab.y) + (3.0f * t * t * br.y) + (t * t * t * as.y),
                        };
                        d1 = {
                            (3.0f * ab.x) + (6.0f * t * br.x) + (3.0f * t * t * as.x),
                            (3.0f * ab.y) + (6.0f * t * br.y) + (3.0f * t * t * as.y),
                        };
                        if (--remainingSteps == 0)
                        {
                            break;
                        }
                        d2 = {
                            (6.0f * br.x) + (6.0f * t * as.x),
                            (6.0f * br.y) + (6.0f * t * as.y),
                        };
                        denominator = Dot(d1, d1) + Dot(qe, d2);
                        if (std::abs(denominator) <= 0.000001f)
                        {
                            break;
                        }
                        improvedT = t - (Dot(qe, d1) / denominator);
                    }
                    while (improvedT > 0.0f && improvedT < 1.0f);

                    const float distance = Length(qe);
                    if (distance < std::abs(minDistance))
                    {
                        minDistance = NonZeroSign(Cross(d1, qe)) * distance;
                        param = t;
                    }
                }
            }

            if (param >= 0.0f && param <= 1.0f)
            {
                result = SignedDistance{ minDistance, 0.0f };
            }
            else if (param < 0.5f)
            {
                result = SignedDistance{ minDistance, std::abs(Dot(Normalize(SegmentDirectionMsdf(segment, 0.0f)), Normalize(qa))) };
            }
            else
            {
                result = SignedDistance{ minDistance, std::abs(Dot(Normalize(SegmentDirectionMsdf(segment, 1.0f)), Normalize(bq))) };
            }
            break;
        }
    }

    if (outParam != nullptr)
    {
        *outParam = param;
    }
    return result;
}

bool GetPerpendicularDistance(float* distance, Vec2 ep, Vec2 edgeDirection)
{
    const float ts = Dot(ep, edgeDirection);
    if (ts > 0.0f)
    {
        const float perpendicularDistance = Cross(ep, edgeDirection);
        if (std::abs(perpendicularDistance) < std::abs(*distance))
        {
            *distance = perpendicularDistance;
            return true;
        }
    }
    return false;
}

void AddPerpendicularDistance(EdgeDistance* selector, float distance)
{
    if (distance <= 0.0f && distance > selector->minNegativePerpendicularDistance)
    {
        selector->minNegativePerpendicularDistance = distance;
    }
    if (distance >= 0.0f && distance < selector->minPositivePerpendicularDistance)
    {
        selector->minPositivePerpendicularDistance = distance;
    }
}

void DistanceToPerpendicularDistance(SignedDistance* distance, Vec2 point, const Segment& segment, float param)
{
    if (param < 0.0f)
    {
        const Vec2 direction = Normalize(SegmentDirection(segment, 0.0f));
        const Vec2 endpointDelta{
            point.x - segment.a.x,
            point.y - segment.a.y,
        };
        if (Dot(endpointDelta, direction) < 0.0f)
        {
            const float perpendicularDistance = Cross(endpointDelta, direction);
            if (std::abs(perpendicularDistance) <= std::abs(distance->distance))
            {
                distance->distance = perpendicularDistance;
            }
        }
    }
    else if (param > 1.0f)
    {
        const Vec2 direction = Normalize(SegmentDirection(segment, 1.0f));
        const Vec2 endpointDelta{
            point.x - segment.b.x,
            point.y - segment.b.y,
        };
        if (Dot(endpointDelta, direction) > 0.0f)
        {
            const float perpendicularDistance = Cross(endpointDelta, direction);
            if (std::abs(perpendicularDistance) <= std::abs(distance->distance))
            {
                distance->distance = perpendicularDistance;
            }
        }
    }
}

float ComputePerpendicularDistance(const EdgeDistance& selector, Vec2 point)
{
    float minDistance = selector.trueDistance.distance < 0.0f
        ? selector.minNegativePerpendicularDistance
        : selector.minPositivePerpendicularDistance;
    if (selector.nearEdge != nullptr)
    {
        SignedDistance distance = selector.trueDistance;
        DistanceToPerpendicularDistance(&distance, point, *selector.nearEdge, selector.nearParam);
        if (std::abs(distance.distance) < std::abs(minDistance))
        {
            minDistance = distance.distance;
        }
    }
    return minDistance;
}

void AddEdgeDistance(
    EdgeDistance* selector,
    Vec2 point,
    const Segment& previous,
    const Segment& segment,
    const Segment& next,
    const SignedDistance& distance,
    float param)
{
    if (distance < selector->trueDistance)
    {
        selector->trueDistance = distance;
        selector->nearEdge = &segment;
        selector->nearParam = param;
    }

    const Vec2 ap{
        point.x - segment.a.x,
        point.y - segment.a.y,
    };
    const Vec2 bp{
        point.x - segment.b.x,
        point.y - segment.b.y,
    };
    const Vec2 aDirection = Normalize(SegmentDirection(segment, 0.0f), true);
    const Vec2 bDirection = Normalize(SegmentDirection(segment, 1.0f), true);
    const Vec2 previousDirection = Normalize(SegmentDirection(previous, 1.0f), true);
    const Vec2 nextDirection = Normalize(SegmentDirection(next, 0.0f), true);
    const Vec2 aDomainDirection = Normalize({
        previousDirection.x + aDirection.x,
        previousDirection.y + aDirection.y,
    }, true);
    const Vec2 bDomainDirection = Normalize({
        bDirection.x + nextDirection.x,
        bDirection.y + nextDirection.y,
    }, true);
    const float aDomainDistance = Dot(ap, aDomainDirection);
    const float bDomainDistance = -Dot(bp, bDomainDirection);
    if (aDomainDistance > 0.0f)
    {
        float perpendicularDistance = distance.distance;
        if (GetPerpendicularDistance(&perpendicularDistance, ap, { -aDirection.x, -aDirection.y }))
        {
            AddPerpendicularDistance(selector, -perpendicularDistance);
        }
    }
    if (bDomainDistance > 0.0f)
    {
        float perpendicularDistance = distance.distance;
        if (GetPerpendicularDistance(&perpendicularDistance, bp, bDirection))
        {
            AddPerpendicularDistance(selector, perpendicularDistance);
        }
    }
}

std::uint32_t InitMsdfEdgeColor(unsigned long long* seed)
{
    static constexpr std::array<std::uint32_t, 3> kColors{
        0x6u, // CYAN
        0x5u, // MAGENTA
        0x3u, // YELLOW
    };
    const int value = static_cast<int>(*seed % 3ull);
    *seed /= 3ull;
    return kColors[static_cast<std::size_t>(value)];
}

void SwitchMsdfEdgeColor(std::uint32_t* color, unsigned long long* seed)
{
    const int shift = 1 + static_cast<int>(*seed & 1ull);
    *seed >>= 1ull;
    const std::uint32_t shifted = *color << shift;
    *color = (shifted | (shifted >> 3u)) & 0x7u;
}

void SwitchMsdfEdgeColor(std::uint32_t* color, unsigned long long* seed, std::uint32_t banned)
{
    const std::uint32_t combined = *color & banned;
    if (combined == 0x1u || combined == 0x2u || combined == 0x4u)
    {
        *color = combined ^ 0x7u;
    }
    else
    {
        SwitchMsdfEdgeColor(color, seed);
    }
}

int SymmetricalTrichotomy(int position, int count)
{
    return static_cast<int>(3.0 + (2.875 * static_cast<double>(position) / static_cast<double>(count - 1)) - 1.4375 + 0.5) - 3;
}

bool IsMsdfCorner(Vec2 aDirection, Vec2 bDirection, float crossThreshold)
{
    return Dot(aDirection, bDirection) <= 0.0f || std::abs(Cross(aDirection, bDirection)) > crossThreshold;
}

Segment SegmentSlice(const Segment& source, float t0, float t1)
{
    if (source.type == Segment::Type::Line)
    {
        Segment segment = source;
        segment.a = SegmentPoint(source, t0);
        segment.b = SegmentPoint(source, t1);
        return segment;
    }

    if (source.type == Segment::Type::Quadratic)
    {
        const Vec2 p00 = source.a;
        const Vec2 p01 = source.control0;
        const Vec2 p02 = source.b;
        const Vec2 p10 = Lerp(p00, p01, t0);
        const Vec2 p11 = Lerp(p01, p02, t0);
        const Vec2 p20 = Lerp(p10, p11, t0);

        const float denom = std::max(1.0f - t0, 0.000001f);
        const float u = (t1 - t0) / denom;
        const Vec2 q10 = Lerp(p10, p11, u);
        const Vec2 q20 = Lerp(p20, q10, u);
        return Segment{
            .type = Segment::Type::Quadratic,
            .a = p20,
            .control0 = q10,
            .b = q20,
            .colorMask = source.colorMask,
        };
    }

    const Vec2 p00 = source.a;
    const Vec2 p01 = source.control0;
    const Vec2 p02 = source.control1;
    const Vec2 p03 = source.b;
    const Vec2 p10 = Lerp(p00, p01, t0);
    const Vec2 p11 = Lerp(p01, p02, t0);
    const Vec2 p12 = Lerp(p02, p03, t0);
    const Vec2 p20 = Lerp(p10, p11, t0);
    const Vec2 p21 = Lerp(p11, p12, t0);
    const Vec2 p30 = Lerp(p20, p21, t0);

    const float denom = std::max(1.0f - t0, 0.000001f);
    const float u = (t1 - t0) / denom;
    const Vec2 q10 = Lerp(p20, p21, u);
    const Vec2 q11 = Lerp(p21, p12, u);
    const Vec2 q20 = Lerp(p30, q10, u);
    const Vec2 q21 = Lerp(q10, q11, u);
    const Vec2 q30 = Lerp(q20, q21, u);
    return Segment{
        .type = Segment::Type::Cubic,
        .a = p30,
        .control0 = q20,
        .control1 = q21,
        .b = q30,
        .colorMask = source.colorMask,
    };
}

void SplitSegmentInThirds(const Segment& source, Segment* first, Segment* second, Segment* third)
{
    *first = SegmentSlice(source, 0.0f, 1.0f / 3.0f);
    *second = SegmentSlice(source, 1.0f / 3.0f, 2.0f / 3.0f);
    *third = SegmentSlice(source, 2.0f / 3.0f, 1.0f);
}

Segment ConvertQuadraticToCubic(const Segment& source)
{
    return Segment{
        .type = Segment::Type::Cubic,
        .a = source.a,
        .control0 = Lerp(source.a, source.control0, 2.0f / 3.0f),
        .control1 = Lerp(source.control0, source.b, 1.0f / 3.0f),
        .b = source.b,
        .colorMask = source.colorMask,
    };
}

int SegmentOrder(const Segment& segment)
{
    switch (segment.type)
    {
    case Segment::Type::Line:
        return 1;
    case Segment::Type::Quadratic:
        return 2;
    case Segment::Type::Cubic:
        return 3;
    }
    return 1;
}

Vec2 SegmentControlPoint(const Segment& segment, int index)
{
    switch (segment.type)
    {
    case Segment::Type::Line:
        return index == 0 ? segment.a : segment.b;
    case Segment::Type::Quadratic:
        if (index == 0)
        {
            return segment.a;
        }
        return index == 1 ? segment.control0 : segment.b;
    case Segment::Type::Cubic:
        if (index == 0)
        {
            return segment.a;
        }
        if (index == 1)
        {
            return segment.control0;
        }
        return index == 2 ? segment.control1 : segment.b;
    }
    return segment.a;
}

bool EqualPoint(Vec2 lhs, Vec2 rhs)
{
    return lhs.x == rhs.x && lhs.y == rhs.y;
}

void SimplifyDegenerateCurve(std::array<Vec2, 4>* controlPoints, int* order)
{
    if (*order == 3 &&
        (EqualPoint((*controlPoints)[1], (*controlPoints)[0]) || EqualPoint((*controlPoints)[1], (*controlPoints)[3])) &&
        (EqualPoint((*controlPoints)[2], (*controlPoints)[0]) || EqualPoint((*controlPoints)[2], (*controlPoints)[3])))
    {
        (*controlPoints)[1] = (*controlPoints)[3];
        *order = 1;
    }
    if (*order == 2 &&
        (EqualPoint((*controlPoints)[1], (*controlPoints)[0]) || EqualPoint((*controlPoints)[1], (*controlPoints)[2])))
    {
        (*controlPoints)[1] = (*controlPoints)[2];
        *order = 1;
    }
    if (*order == 1 && EqualPoint((*controlPoints)[0], (*controlPoints)[1]))
    {
        *order = 0;
    }
}

int ConvergentCurveOrdering(const std::array<Vec2, 12>& controlPoints, int controlPointsBefore, int controlPointsAfter)
{
    if (!(controlPointsBefore > 0 && controlPointsAfter > 0))
    {
        return 0;
    }
    const int cornerIndex = 4;
    Vec2 a1{ controlPoints[cornerIndex - 1].x - controlPoints[cornerIndex].x, controlPoints[cornerIndex - 1].y - controlPoints[cornerIndex].y };
    Vec2 b1{ controlPoints[cornerIndex + 1].x - controlPoints[cornerIndex].x, controlPoints[cornerIndex + 1].y - controlPoints[cornerIndex].y };
    Vec2 a2{};
    Vec2 a3{};
    Vec2 b2{};
    Vec2 b3{};
    if (controlPointsBefore >= 2)
    {
        a2 = {
            controlPoints[cornerIndex - 2].x - controlPoints[cornerIndex - 1].x - a1.x,
            controlPoints[cornerIndex - 2].y - controlPoints[cornerIndex - 1].y - a1.y,
        };
    }
    if (controlPointsAfter >= 2)
    {
        b2 = {
            controlPoints[cornerIndex + 2].x - controlPoints[cornerIndex + 1].x - b1.x,
            controlPoints[cornerIndex + 2].y - controlPoints[cornerIndex + 1].y - b1.y,
        };
    }
    if (controlPointsBefore >= 3)
    {
        const Vec2 prevDelta{
            controlPoints[cornerIndex - 2].x - controlPoints[cornerIndex - 1].x,
            controlPoints[cornerIndex - 2].y - controlPoints[cornerIndex - 1].y,
        };
        a3 = {
            controlPoints[cornerIndex - 3].x - controlPoints[cornerIndex - 2].x - prevDelta.x - a2.x,
            controlPoints[cornerIndex - 3].y - controlPoints[cornerIndex - 2].y - prevDelta.y - a2.y,
        };
        a2.x *= 3.0f;
        a2.y *= 3.0f;
    }
    if (controlPointsAfter >= 3)
    {
        const Vec2 nextDelta{
            controlPoints[cornerIndex + 2].x - controlPoints[cornerIndex + 1].x,
            controlPoints[cornerIndex + 2].y - controlPoints[cornerIndex + 1].y,
        };
        b3 = {
            controlPoints[cornerIndex + 3].x - controlPoints[cornerIndex + 2].x - nextDelta.x - b2.x,
            controlPoints[cornerIndex + 3].y - controlPoints[cornerIndex + 2].y - nextDelta.y - b2.y,
        };
        b2.x *= 3.0f;
        b2.y *= 3.0f;
    }
    a1.x *= static_cast<float>(controlPointsBefore);
    a1.y *= static_cast<float>(controlPointsBefore);
    b1.x *= static_cast<float>(controlPointsAfter);
    b1.y *= static_cast<float>(controlPointsAfter);

    if (Length(a1) > 0.0f && Length(b1) > 0.0f)
    {
        const float as = Length(a1);
        const float bs = Length(b1);
        if (const float d = (as * Cross(a1, b2)) + (bs * Cross(a2, b1)); d != 0.0f)
        {
            return Sign(d);
        }
        if (const float d = (as * as * Cross(a1, b3)) + (as * bs * Cross(a2, b2)) + (bs * bs * Cross(a3, b1)); d != 0.0f)
        {
            return Sign(d);
        }
        if (const float d = (as * Cross(a2, b3)) + (bs * Cross(a3, b2)); d != 0.0f)
        {
            return Sign(d);
        }
        return Sign(Cross(a3, b3));
    }

    int s = 1;
    if (Length(a1) > 0.0f)
    {
        b1 = a1;
        a1 = b2;
        b2 = a2;
        a2 = a1;
        a1 = b3;
        b3 = a3;
        a3 = a1;
        s = -1;
    }
    if (Length(b1) > 0.0f)
    {
        if (const float d = Cross(a3, b1); d != 0.0f)
        {
            return s * Sign(d);
        }
        if (const float d = Cross(a2, b2); d != 0.0f)
        {
            return s * Sign(d);
        }
        if (const float d = Cross(a3, b2); d != 0.0f)
        {
            return s * Sign(d);
        }
        if (const float d = Cross(a2, b3); d != 0.0f)
        {
            return s * Sign(d);
        }
        return s * Sign(Cross(a3, b3));
    }

    if (const float d = (std::sqrt(Length(a2)) * Cross(a2, b3)) + (std::sqrt(Length(b2)) * Cross(a3, b2)); d != 0.0f)
    {
        return Sign(d);
    }
    return Sign(Cross(a3, b3));
}

int ConvergentCurveOrdering(const Segment& previous, const Segment& current)
{
    std::array<Vec2, 12> controlPoints{};
    constexpr int kCornerIndex = 4;
    std::array<Vec2, 4> previousPoints{};
    std::array<Vec2, 4> currentPoints{};
    int previousOrder = SegmentOrder(previous);
    int currentOrder = SegmentOrder(current);
    if (!(previousOrder >= 1 && previousOrder <= 3 && currentOrder >= 1 && currentOrder <= 3))
    {
        return 0;
    }
    for (int index = 0; index <= previousOrder; ++index)
    {
        previousPoints[index] = SegmentControlPoint(previous, index);
    }
    for (int index = 0; index <= currentOrder; ++index)
    {
        currentPoints[index] = SegmentControlPoint(current, index);
        controlPoints[kCornerIndex + index] = currentPoints[index];
    }
    if (!EqualPoint(previousPoints[previousOrder], controlPoints[kCornerIndex]))
    {
        return 0;
    }
    SimplifyDegenerateCurve(&previousPoints, &previousOrder);
    SimplifyDegenerateCurve(&currentPoints, &currentOrder);
    for (int index = 0; index < previousOrder; ++index)
    {
        controlPoints[kCornerIndex + index - previousOrder] = previousPoints[index];
    }
    for (int index = 0; index <= currentOrder; ++index)
    {
        controlPoints[kCornerIndex + index] = currentPoints[index];
    }
    return ConvergentCurveOrdering(controlPoints, previousOrder, currentOrder);
}

Vec2 Orthogonal(Vec2 value, bool polarity)
{
    return polarity ? Vec2{ -value.y, value.x } : Vec2{ value.y, -value.x };
}

void DeconvergeEdge(Segment* segment, int param, Vec2 vector)
{
    if (segment->type == Segment::Type::Quadratic)
    {
        *segment = ConvertQuadraticToCubic(*segment);
    }
    if (segment->type != Segment::Type::Cubic)
    {
        return;
    }

    if (param == 0)
    {
        const Vec2 delta{
            segment->control0.x - segment->a.x,
            segment->control0.y - segment->a.y,
        };
        const float length = Length(delta);
        segment->control0.x += length * vector.x;
        segment->control0.y += length * vector.y;
    }
    else if (param == 1)
    {
        const Vec2 delta{
            segment->control1.x - segment->b.x,
            segment->control1.y - segment->b.y,
        };
        const float length = Length(delta);
        segment->control1.x += length * vector.x;
        segment->control1.y += length * vector.y;
    }
}

void NormalizeShape(GlyphShape* shape)
{
    for (std::vector<std::size_t>& contourSegments : shape->contourSegmentIndices)
    {
        if (contourSegments.size() == 1u)
        {
            const Segment original = shape->segments[contourSegments.front()];
            Segment first;
            Segment second;
            Segment third;
            SplitSegmentInThirds(original, &first, &second, &third);
            contourSegments.clear();
            shape->segments.push_back(first);
            contourSegments.push_back(shape->segments.size() - 1u);
            shape->segments.push_back(second);
            contourSegments.push_back(shape->segments.size() - 1u);
            shape->segments.push_back(third);
            contourSegments.push_back(shape->segments.size() - 1u);
        }
        else if (!contourSegments.empty())
        {
            std::size_t previousIndex = contourSegments.back();
            for (std::size_t segmentIndex : contourSegments)
            {
                Segment& previous = shape->segments[previousIndex];
                Segment& current = shape->segments[segmentIndex];
                const Vec2 previousDirection = Normalize(SegmentDirection(previous, 1.0f));
                const Vec2 currentDirection = Normalize(SegmentDirection(current, 0.0f));
                constexpr float kCornerDotEpsilon = 0.000001f;
                if (Dot(previousDirection, currentDirection) < kCornerDotEpsilon - 1.0f)
                {
                    constexpr float kDeconvergeOvershoot = 1.11111111111111111f;
                    const float limit = kCornerDotEpsilon - 1.0f;
                    const float factor = kDeconvergeOvershoot * std::sqrt(1.0f - (limit * limit)) / limit;
                    Vec2 axis = Normalize({
                        currentDirection.x - previousDirection.x,
                        currentDirection.y - previousDirection.y,
                    });
                    axis.x *= factor;
                    axis.y *= factor;
                    if (ConvergentCurveOrdering(previous, current) < 0)
                    {
                        axis.x = -axis.x;
                        axis.y = -axis.y;
                    }
                    DeconvergeEdge(&previous, 1, Orthogonal(axis, true));
                    DeconvergeEdge(&current, 0, Orthogonal(axis, false));
                }
                previousIndex = segmentIndex;
            }
        }
    }
}

void ColorShapeEdges(GlyphShape* shape)
{
    constexpr float kCornerAngleRadians = 3.0f;
    const float crossThreshold = std::sin(kCornerAngleRadians);
    unsigned long long seed = 0ull;
    std::uint32_t color = InitMsdfEdgeColor(&seed);

    for (std::vector<std::size_t>& contourSegments : shape->contourSegmentIndices)
    {
        if (contourSegments.empty())
        {
            continue;
        }

        std::vector<std::size_t> cornerSegmentOffsets;
        for (std::size_t offset = 0; offset < contourSegments.size(); ++offset)
        {
            const Segment& previous = shape->segments[contourSegments[(offset + contourSegments.size() - 1u) % contourSegments.size()]];
            const Segment& current = shape->segments[contourSegments[offset]];
            const Vec2 previousDirection = Normalize(SegmentDirection(previous, 1.0f));
            const Vec2 currentDirection = Normalize(SegmentDirection(current, 0.0f));
            if (IsMsdfCorner(previousDirection, currentDirection, crossThreshold))
            {
                cornerSegmentOffsets.push_back(offset);
            }
        }

        if (cornerSegmentOffsets.empty())
        {
            SwitchMsdfEdgeColor(&color, &seed);
            for (const std::size_t segmentIndex : contourSegments)
            {
                shape->segments[segmentIndex].colorMask = color;
            }
            continue;
        }

        if (cornerSegmentOffsets.size() == 1u)
        {
            SwitchMsdfEdgeColor(&color, &seed);
            const std::uint32_t firstColor = color;
            SwitchMsdfEdgeColor(&color, &seed);
            const std::uint32_t secondColor = color;
            const std::array<std::uint32_t, 3> colors{ firstColor, 0x7u, secondColor };
            const std::size_t corner = cornerSegmentOffsets.front();
            for (std::size_t index = 0; index < contourSegments.size(); ++index)
            {
                const int section = SymmetricalTrichotomy(static_cast<int>(index), static_cast<int>(contourSegments.size()));
                shape->segments[contourSegments[(corner + index) % contourSegments.size()]].colorMask = colors[1 + section];
            }
            continue;
        }

        const std::size_t cornerCount = cornerSegmentOffsets.size();
        std::size_t spline = 0;
        const std::size_t start = cornerSegmentOffsets.front();
        SwitchMsdfEdgeColor(&color, &seed);
        const std::uint32_t initialColor = color;
        for (std::size_t index = 0; index < contourSegments.size(); ++index)
        {
            const std::size_t segmentOffset = (start + index) % contourSegments.size();
            if (spline + 1u < cornerCount && cornerSegmentOffsets[spline + 1u] == segmentOffset)
            {
                ++spline;
                SwitchMsdfEdgeColor(&color, &seed, spline == cornerCount - 1u ? initialColor : 0u);
            }
            shape->segments[contourSegments[segmentOffset]].colorMask = color;
        }
    }
}

int SegmentScanlineIntersections(const Segment& segment, float y, float* intersections, int* directions)
{
    switch (segment.type)
    {
    case Segment::Type::Line:
        if ((y >= segment.a.y && y < segment.b.y) || (y >= segment.b.y && y < segment.a.y))
        {
            const float t = (y - segment.a.y) / (segment.b.y - segment.a.y);
            intersections[0] = Lerp(segment.a, segment.b, t).x;
            directions[0] = Sign(segment.b.y - segment.a.y);
            return 1;
        }
        return 0;
    case Segment::Type::Quadratic:
        {
            int total = 0;
            int nextDirection = y > segment.a.y ? 1 : -1;
            intersections[total] = segment.a.x;
            if (segment.a.y == y)
            {
                if (segment.a.y < segment.control0.y || (segment.a.y == segment.control0.y && segment.a.y < segment.b.y))
                {
                    directions[total++] = 1;
                }
                else
                {
                    nextDirection = 1;
                }
            }

            const Vec2 ab{ segment.control0.x - segment.a.x, segment.control0.y - segment.a.y };
            const Vec2 br{ segment.b.x - segment.control0.x - ab.x, segment.b.y - segment.control0.y - ab.y };
            float roots[2]{};
            int rootCount = SolveQuadratic(roots, br.y, 2.0f * ab.y, segment.a.y - y);
            if (rootCount >= 2 && roots[0] > roots[1])
            {
                std::swap(roots[0], roots[1]);
            }
            for (int index = 0; index < rootCount && total < 2; ++index)
            {
                const float t = roots[index];
                if (t >= 0.0f && t <= 1.0f)
                {
                    intersections[total] = segment.a.x + (2.0f * t * ab.x) + (t * t * br.x);
                    if (nextDirection * (ab.y + (t * br.y)) >= 0.0f)
                    {
                        directions[total++] = nextDirection;
                        nextDirection = -nextDirection;
                    }
                }
            }
            if (segment.b.y == y)
            {
                if (nextDirection > 0 && total > 0)
                {
                    --total;
                    nextDirection = -1;
                }
                if ((segment.b.y < segment.control0.y || (segment.b.y == segment.control0.y && segment.b.y < segment.a.y)) && total < 2)
                {
                    intersections[total] = segment.b.x;
                    if (nextDirection < 0)
                    {
                        directions[total++] = -1;
                        nextDirection = 1;
                    }
                }
            }
            if (nextDirection != (y >= segment.b.y ? 1 : -1))
            {
                if (total > 0)
                {
                    --total;
                }
                else
                {
                    if (std::abs(segment.b.y - y) < std::abs(segment.a.y - y))
                    {
                        intersections[total] = segment.b.x;
                    }
                    directions[total++] = nextDirection;
                }
            }
            return total;
        }
    case Segment::Type::Cubic:
        {
            int total = 0;
            int nextDirection = y > segment.a.y ? 1 : -1;
            intersections[total] = segment.a.x;
            if (segment.a.y == y)
            {
                if (segment.a.y < segment.control0.y ||
                    (segment.a.y == segment.control0.y &&
                        (segment.a.y < segment.control1.y || (segment.a.y == segment.control1.y && segment.a.y < segment.b.y))))
                {
                    directions[total++] = 1;
                }
                else
                {
                    nextDirection = 1;
                }
            }

            const Vec2 ab{ segment.control0.x - segment.a.x, segment.control0.y - segment.a.y };
            const Vec2 br{ segment.control1.x - segment.control0.x - ab.x, segment.control1.y - segment.control0.y - ab.y };
            const Vec2 as{
                segment.b.x - (3.0f * segment.control1.x) + (3.0f * segment.control0.x) - segment.a.x,
                segment.b.y - (3.0f * segment.control1.y) + (3.0f * segment.control0.y) - segment.a.y,
            };
            float roots[3]{};
            int rootCount = SolveCubic(roots, as.y, 3.0f * br.y, 3.0f * ab.y, segment.a.y - y);
            std::sort(roots, roots + rootCount);
            for (int index = 0; index < rootCount && total < 3; ++index)
            {
                const float t = roots[index];
                if (t >= 0.0f && t <= 1.0f)
                {
                    intersections[total] = segment.a.x + (3.0f * t * ab.x) + (3.0f * t * t * br.x) + (t * t * t * as.x);
                    if (nextDirection * (ab.y + (2.0f * t * br.y) + (t * t * as.y)) >= 0.0f)
                    {
                        directions[total++] = nextDirection;
                        nextDirection = -nextDirection;
                    }
                }
            }
            if (segment.b.y == y)
            {
                if (nextDirection > 0 && total > 0)
                {
                    --total;
                    nextDirection = -1;
                }
                if ((segment.b.y < segment.control1.y ||
                        (segment.b.y == segment.control1.y &&
                            (segment.b.y < segment.control0.y || (segment.b.y == segment.control0.y && segment.b.y < segment.a.y)))) &&
                    total < 3)
                {
                    intersections[total] = segment.b.x;
                    if (nextDirection < 0)
                    {
                        directions[total++] = -1;
                        nextDirection = 1;
                    }
                }
            }
            if (nextDirection != (y >= segment.b.y ? 1 : -1))
            {
                if (total > 0)
                {
                    --total;
                }
                else
                {
                    if (std::abs(segment.b.y - y) < std::abs(segment.a.y - y))
                    {
                        intersections[total] = segment.b.x;
                    }
                    directions[total++] = nextDirection;
                }
            }
            return total;
        }
    }
    return 0;
}

bool PointInShape(Vec2 point, const GlyphShape& shape)
{
    struct Intersection
    {
        float x = 0.0f;
        int direction = 0;
    };
    std::vector<Intersection> intersections;
    for (const Segment& segment : shape.segments)
    {
        float segmentIntersections[3]{};
        int directions[3]{};
        const int intersectionCount = SegmentScanlineIntersections(segment, point.y, segmentIntersections, directions);
        for (int index = 0; index < intersectionCount; ++index)
        {
            intersections.push_back(Intersection{ segmentIntersections[index], directions[index] });
        }
    }

    std::sort(intersections.begin(), intersections.end(), [](const Intersection& lhs, const Intersection& rhs) {
        return lhs.x < rhs.x;
    });

    int windingNumber = 0;
    for (const Intersection& intersection : intersections)
    {
        if (point.x < intersection.x)
        {
            break;
        }
        windingNumber += intersection.direction;
    }
    return windingNumber != 0;
}

float Shoelace(Vec2 a, Vec2 b)
{
    return (b.x - a.x) * (a.y + b.y);
}

int Sign(float value)
{
    return (value > 0.0f) - (value < 0.0f);
}

int ContourWinding(const GlyphShape& shape, const std::vector<std::size_t>& contourSegments)
{
    if (contourSegments.empty())
    {
        return 0;
    }

    float total = 0.0f;
    if (contourSegments.size() == 1u)
    {
        const Segment& edge = shape.segments[contourSegments[0]];
        const Vec2 a = SegmentPoint(edge, 0.0f);
        const Vec2 b = SegmentPoint(edge, 1.0f / 3.0f);
        const Vec2 c = SegmentPoint(edge, 2.0f / 3.0f);
        total += Shoelace(a, b);
        total += Shoelace(b, c);
        total += Shoelace(c, a);
    }
    else if (contourSegments.size() == 2u)
    {
        const Segment& first = shape.segments[contourSegments[0]];
        const Segment& second = shape.segments[contourSegments[1]];
        const Vec2 a = SegmentPoint(first, 0.0f);
        const Vec2 b = SegmentPoint(first, 0.5f);
        const Vec2 c = SegmentPoint(second, 0.0f);
        const Vec2 d = SegmentPoint(second, 0.5f);
        total += Shoelace(a, b);
        total += Shoelace(b, c);
        total += Shoelace(c, d);
        total += Shoelace(d, a);
    }
    else
    {
        Vec2 previous = SegmentPoint(shape.segments[contourSegments.back()], 0.0f);
        for (std::size_t segmentIndex : contourSegments)
        {
            const Vec2 current = SegmentPoint(shape.segments[segmentIndex], 0.0f);
            total += Shoelace(previous, current);
            previous = current;
        }
    }
    return Sign(total);
}

std::array<float, 3> EdgeDistanceValue(const std::array<EdgeDistance, 3>& selector, Vec2 point, SignedDistance fallbackDistance)
{
    std::array<float, 3> result{};
    for (std::size_t channel = 0; channel < 3u; ++channel)
    {
        result[channel] = selector[channel].nearEdge != nullptr
            ? ComputePerpendicularDistance(selector[channel], point)
            : selector[channel].trueDistance.distance;
    }
    return result;
}

float ResolveMsdfDistance(const std::array<float, 3>& distance)
{
    return Median(distance[0], distance[1], distance[2]);
}

bool IsInitializedMsdfDistance(const std::array<float, 3>& distance)
{
    return distance[0] != -std::numeric_limits<float>::max() ||
        distance[1] != -std::numeric_limits<float>::max() ||
        distance[2] != -std::numeric_limits<float>::max();
}

void MergeEdgeSelector(std::array<EdgeDistance, 3>* target, const std::array<EdgeDistance, 3>& source)
{
    for (std::size_t channel = 0; channel < 3u; ++channel)
    {
        EdgeDistance& lhs = (*target)[channel];
        const EdgeDistance& rhs = source[channel];
        if (rhs.trueDistance < lhs.trueDistance)
        {
            lhs.trueDistance = rhs.trueDistance;
            lhs.nearEdge = rhs.nearEdge;
            lhs.nearParam = rhs.nearParam;
        }
        if (rhs.minNegativePerpendicularDistance > lhs.minNegativePerpendicularDistance)
        {
            lhs.minNegativePerpendicularDistance = rhs.minNegativePerpendicularDistance;
        }
        if (rhs.minPositivePerpendicularDistance < lhs.minPositivePerpendicularDistance)
        {
            lhs.minPositivePerpendicularDistance = rhs.minPositivePerpendicularDistance;
        }
    }
}

float EdgeDistanceValue(const EdgeDistance& selector, Vec2 point)
{
    return selector.nearEdge != nullptr
        ? ComputePerpendicularDistance(selector, point)
        : selector.trueDistance.distance;
}

void MergeEdgeSelector(EdgeDistance* target, const EdgeDistance& source)
{
    if (source.trueDistance < target->trueDistance)
    {
        target->trueDistance = source.trueDistance;
        target->nearEdge = source.nearEdge;
        target->nearParam = source.nearParam;
    }
    if (source.minNegativePerpendicularDistance > target->minNegativePerpendicularDistance)
    {
        target->minNegativePerpendicularDistance = source.minNegativePerpendicularDistance;
    }
    if (source.minPositivePerpendicularDistance < target->minPositivePerpendicularDistance)
    {
        target->minPositivePerpendicularDistance = source.minPositivePerpendicularDistance;
    }
}

float ResolveOverlappingContourPerpendicularDistance(
    const std::vector<EdgeDistance>& contourSelectors,
    const std::vector<int>& contourWindings,
    Vec2 point)
{
    EdgeDistance shapeSelector{};
    EdgeDistance innerSelector{};
    EdgeDistance outerSelector{};
    for (std::size_t index = 0; index < contourSelectors.size(); ++index)
    {
        const float edgeDistance = EdgeDistanceValue(contourSelectors[index], point);
        MergeEdgeSelector(&shapeSelector, contourSelectors[index]);
        if (contourWindings[index] > 0 && edgeDistance >= 0.0f)
        {
            MergeEdgeSelector(&innerSelector, contourSelectors[index]);
        }
        if (contourWindings[index] < 0 && edgeDistance <= 0.0f)
        {
            MergeEdgeSelector(&outerSelector, contourSelectors[index]);
        }
    }

    const float shapeDistance = EdgeDistanceValue(shapeSelector, point);
    const float innerDistance = EdgeDistanceValue(innerSelector, point);
    const float outerDistance = EdgeDistanceValue(outerSelector, point);
    float distance = -std::numeric_limits<float>::max();
    int winding = 0;
    if (innerDistance >= 0.0f && std::abs(innerDistance) <= std::abs(outerDistance))
    {
        distance = innerDistance;
        winding = 1;
        for (std::size_t index = 0; index < contourSelectors.size(); ++index)
        {
            if (contourWindings[index] > 0)
            {
                const float contourDistance = EdgeDistanceValue(contourSelectors[index], point);
                if (std::abs(contourDistance) < std::abs(outerDistance) && contourDistance > distance)
                {
                    distance = contourDistance;
                }
            }
        }
    }
    else if (outerDistance <= 0.0f && std::abs(outerDistance) < std::abs(innerDistance))
    {
        distance = outerDistance;
        winding = -1;
        for (std::size_t index = 0; index < contourSelectors.size(); ++index)
        {
            if (contourWindings[index] < 0)
            {
                const float contourDistance = EdgeDistanceValue(contourSelectors[index], point);
                if (std::abs(contourDistance) < std::abs(innerDistance) && contourDistance < distance)
                {
                    distance = contourDistance;
                }
            }
        }
    }
    else
    {
        return shapeDistance;
    }

    for (std::size_t index = 0; index < contourSelectors.size(); ++index)
    {
        if (contourWindings[index] != winding)
        {
            const float contourDistance = EdgeDistanceValue(contourSelectors[index], point);
            if (contourDistance * distance >= 0.0f && std::abs(contourDistance) < std::abs(distance))
            {
                distance = contourDistance;
            }
        }
    }
    if (distance == shapeDistance)
    {
        distance = shapeDistance;
    }
    return distance;
}

float ExactPerpendicularShapeDistance(const GlyphShape& shape, const std::vector<int>& contourWindings, Vec2 point)
{
    std::vector<EdgeDistance> contourSelectors(shape.contourSegmentIndices.size());
    for (std::size_t contourIndex = 0; contourIndex < shape.contourSegmentIndices.size(); ++contourIndex)
    {
        const std::vector<std::size_t>& contourSegments = shape.contourSegmentIndices[contourIndex];
        if (contourSegments.empty())
        {
            continue;
        }
        for (std::size_t offset = 0; offset < contourSegments.size(); ++offset)
        {
            const Segment& segment = shape.segments[contourSegments[offset]];
            const Segment& previous = shape.segments[contourSegments[(offset + contourSegments.size() - 1u) % contourSegments.size()]];
            const Segment& next = shape.segments[contourSegments[(offset + 1u) % contourSegments.size()]];
            float param = 0.0f;
            const SignedDistance signedDistance = SignedDistanceToSegment(point, segment, &param);
            AddEdgeDistance(&contourSelectors[contourIndex], point, previous, segment, next, signedDistance, param);
        }
    }
    return ResolveOverlappingContourPerpendicularDistance(contourSelectors, contourWindings, point);
}

std::array<float, 3> ResolveOverlappingContourDistance(
    const std::vector<std::array<EdgeDistance, 3>>& contourSelectors,
    const std::vector<int>& contourWindings,
    Vec2 point,
    SignedDistance fallbackDistance)
{
    std::array<EdgeDistance, 3> shapeSelector{};
    std::array<EdgeDistance, 3> innerSelector{};
    std::array<EdgeDistance, 3> outerSelector{};
    for (std::size_t index = 0; index < contourSelectors.size(); ++index)
    {
        const std::array<float, 3> edgeDistance = EdgeDistanceValue(contourSelectors[index], point, fallbackDistance);
        MergeEdgeSelector(&shapeSelector, contourSelectors[index]);
        if (contourWindings[index] > 0 && ResolveMsdfDistance(edgeDistance) >= 0.0f)
        {
            MergeEdgeSelector(&innerSelector, contourSelectors[index]);
        }
        if (contourWindings[index] < 0 && ResolveMsdfDistance(edgeDistance) <= 0.0f)
        {
            MergeEdgeSelector(&outerSelector, contourSelectors[index]);
        }
    }

    const std::array<float, 3> shapeDistance = EdgeDistanceValue(shapeSelector, point, fallbackDistance);
    const std::array<float, 3> innerDistance = EdgeDistanceValue(innerSelector, point, fallbackDistance);
    const std::array<float, 3> outerDistance = EdgeDistanceValue(outerSelector, point, fallbackDistance);
    const float innerScalarDistance = ResolveMsdfDistance(innerDistance);
    const float outerScalarDistance = ResolveMsdfDistance(outerDistance);

    std::array<float, 3> distance{
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
    };
    int winding = 0;
    if (innerScalarDistance >= 0.0f && std::abs(innerScalarDistance) <= std::abs(outerScalarDistance))
    {
        distance = innerDistance;
        winding = 1;
        for (std::size_t index = 0; index < contourSelectors.size(); ++index)
        {
            if (contourWindings[index] > 0)
            {
                const std::array<float, 3> contourDistance = EdgeDistanceValue(contourSelectors[index], point, fallbackDistance);
                if (std::abs(ResolveMsdfDistance(contourDistance)) < std::abs(outerScalarDistance) &&
                    ResolveMsdfDistance(contourDistance) > ResolveMsdfDistance(distance))
                {
                    distance = contourDistance;
                }
            }
        }
    }
    else if (outerScalarDistance <= 0.0f && std::abs(outerScalarDistance) < std::abs(innerScalarDistance))
    {
        distance = outerDistance;
        winding = -1;
        for (std::size_t index = 0; index < contourSelectors.size(); ++index)
        {
            if (contourWindings[index] < 0)
            {
                const std::array<float, 3> contourDistance = EdgeDistanceValue(contourSelectors[index], point, fallbackDistance);
                if (std::abs(ResolveMsdfDistance(contourDistance)) < std::abs(innerScalarDistance) &&
                    ResolveMsdfDistance(contourDistance) < ResolveMsdfDistance(distance))
                {
                    distance = contourDistance;
                }
            }
        }
    }
    else
    {
        return shapeDistance;
    }

    for (std::size_t index = 0; index < contourSelectors.size(); ++index)
    {
        if (contourWindings[index] != winding)
        {
            const std::array<float, 3> contourDistance = EdgeDistanceValue(contourSelectors[index], point, fallbackDistance);
            if (ResolveMsdfDistance(contourDistance) * ResolveMsdfDistance(distance) >= 0.0f &&
                std::abs(ResolveMsdfDistance(contourDistance)) < std::abs(ResolveMsdfDistance(distance)))
            {
                distance = contourDistance;
            }
        }
    }
    if (!IsInitializedMsdfDistance(distance) || ResolveMsdfDistance(distance) == ResolveMsdfDistance(shapeDistance))
    {
        distance = shapeDistance;
    }
    return distance;
}

std::uint8_t EncodeDistance(float signedDistance)
{
    const float normalized = 0.5f + (signedDistance / (2.0f * kDistanceRangePixels));
    return static_cast<std::uint8_t>(std::clamp(normalized * 255.0f, 0.0f, 255.0f));
}

float Median(float a, float b, float c)
{
    return std::max(std::min(a, b), std::min(std::max(a, b), c));
}

enum MsdfErrorStencil : std::uint8_t
{
    MsdfErrorStencilProtected = 0x01u,
    MsdfErrorStencilError = 0x02u,
};

struct MsdfErrorContext
{
    const std::vector<std::array<float, 3>>* pixels = nullptr;
    const GlyphShape* shape = nullptr;
    const std::vector<int>* contourWindings = nullptr;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    float minX = 0.0f;
    float maxY = 0.0f;
};

std::array<float, 3> InterpolateMsdf(const MsdfErrorContext& context, Vec2 sdfCoord)
{
    const float pixelX = sdfCoord.x - 0.5f;
    const float pixelY = sdfCoord.y - 0.5f;
    const int x0 = std::clamp(static_cast<int>(std::floor(pixelX)), 0, static_cast<int>(context.width) - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(pixelY)), 0, static_cast<int>(context.height) - 1);
    const int x1 = std::clamp(x0 + 1, 0, static_cast<int>(context.width) - 1);
    const int y1 = std::clamp(y0 + 1, 0, static_cast<int>(context.height) - 1);
    const float tx = std::clamp(pixelX - static_cast<float>(x0), 0.0f, 1.0f);
    const float ty = std::clamp(pixelY - static_cast<float>(y0), 0.0f, 1.0f);
    auto pixel = [&](int x, int y) -> const std::array<float, 3>& {
        return (*context.pixels)[static_cast<std::size_t>(y) * context.width + static_cast<std::size_t>(x)];
    };
    const std::array<float, 3>& a = pixel(x0, y0);
    const std::array<float, 3>& b = pixel(x1, y0);
    const std::array<float, 3>& c = pixel(x0, y1);
    const std::array<float, 3>& d = pixel(x1, y1);
    std::array<float, 3> result{};
    for (std::size_t channel = 0; channel < 3u; ++channel)
    {
        const float bottom = a[channel] + ((b[channel] - a[channel]) * tx);
        const float top = c[channel] + ((d[channel] - c[channel]) * tx);
        result[channel] = bottom + ((top - bottom) * ty);
    }
    return result;
}

Vec2 ShapePointFromSdfCoord(const MsdfErrorContext& context, Vec2 sdfCoord)
{
    return Vec2{
        context.minX + (sdfCoord.x - kPaddingPixels),
        context.maxY - (sdfCoord.y - kPaddingPixels),
    };
}

struct MsdfArtifactClassifier
{
    const MsdfErrorContext* context = nullptr;
    const std::array<float, 3>* msd = nullptr;
    Vec2 sdfCoord{};
    Vec2 direction{};
    float span = 0.0f;
    bool protectedFlag = false;

    int rangeTest(float at, float bt, float xt, float am, float bm, float xm) const
    {
        constexpr int kCandidate = 0x01;
        constexpr int kArtifact = 0x02;
        if ((am > 0.5f && bm > 0.5f && xm <= 0.5f) ||
            (am < 0.5f && bm < 0.5f && xm >= 0.5f) ||
            (!protectedFlag && Median(am, bm, xm) != xm))
        {
            const float axSpan = (xt - at) * span;
            const float bxSpan = (bt - xt) * span;
            if (!(xm >= am - axSpan && xm <= am + axSpan && xm >= bm - bxSpan && xm <= bm + bxSpan))
            {
                return kCandidate | kArtifact;
            }
            return kCandidate;
        }
        return 0;
    }

    bool evaluate(float t, float, int flags) const
    {
        constexpr int kCandidate = 0x01;
        constexpr int kArtifact = 0x02;
        if ((flags & kCandidate) == 0)
        {
            return false;
        }
        if ((flags & kArtifact) != 0)
        {
            return true;
        }
        if (context == nullptr || msd == nullptr || context->shape == nullptr || context->contourWindings == nullptr)
        {
            return false;
        }

        const Vec2 tVector{ t * direction.x, t * direction.y };
        const Vec2 sampleSdfCoord{ sdfCoord.x + tVector.x, sdfCoord.y + tVector.y };
        const std::array<float, 3> oldMsd = InterpolateMsdf(*context, sampleSdfCoord);
        const float weight = (1.0f - std::abs(tVector.x)) * (1.0f - std::abs(tVector.y));
        const float currentMedian = Median((*msd)[0], (*msd)[1], (*msd)[2]);
        std::array<float, 3> newMsd{};
        for (std::size_t channel = 0; channel < 3u; ++channel)
        {
            newMsd[channel] = oldMsd[channel] + (weight * (currentMedian - (*msd)[channel]));
        }
        const float oldDistance = Median(oldMsd[0], oldMsd[1], oldMsd[2]);
        const float newDistance = Median(newMsd[0], newMsd[1], newMsd[2]);
        const Vec2 sampleShapePoint = ShapePointFromSdfCoord(*context, sampleSdfCoord);
        const float referenceDistance = 0.5f + (ExactPerpendicularShapeDistance(*context->shape, *context->contourWindings, sampleShapePoint) / (2.0f * kDistanceRangePixels));
        constexpr float kMinImproveRatio = 1.11111111111111111f;
        return kMinImproveRatio * std::abs(newDistance - referenceDistance) < std::abs(oldDistance - referenceDistance);
    }
};

float InterpolatedMedian(const std::array<float, 3>& a, const std::array<float, 3>& b, float t)
{
    return Median(
        a[0] + ((b[0] - a[0]) * t),
        a[1] + ((b[1] - a[1]) * t),
        a[2] + ((b[2] - a[2]) * t));
}

float InterpolatedMedian(const std::array<float, 3>& a, const std::array<float, 3>& l, const std::array<float, 3>& q, float t)
{
    return Median(
        t * ((t * q[0]) + l[0]) + a[0],
        t * ((t * q[1]) + l[1]) + a[1],
        t * ((t * q[2]) + l[2]) + a[2]);
}

bool HasLinearArtifactInner(
    const MsdfArtifactClassifier& classifier,
    float am,
    float bm,
    const std::array<float, 3>& a,
    const std::array<float, 3>& b,
    float dA,
    float dB)
{
    const float denominator = dA - dB;
    if (std::abs(denominator) <= 0.000001f)
    {
        return false;
    }
    const float t = dA / denominator;
    if (t > 0.01f && t < 0.99f)
    {
        const float xm = InterpolatedMedian(a, b, t);
        return classifier.evaluate(t, xm, classifier.rangeTest(0.0f, 1.0f, t, am, bm, xm));
    }
    return false;
}

bool HasLinearArtifact(
    const MsdfArtifactClassifier& classifier,
    float am,
    const std::array<float, 3>& a,
    const std::array<float, 3>& b)
{
    const float bm = Median(b[0], b[1], b[2]);
    return std::abs(am - 0.5f) >= std::abs(bm - 0.5f) &&
        (HasLinearArtifactInner(classifier, am, bm, a, b, a[1] - a[0], b[1] - b[0]) ||
            HasLinearArtifactInner(classifier, am, bm, a, b, a[2] - a[1], b[2] - b[1]) ||
            HasLinearArtifactInner(classifier, am, bm, a, b, a[0] - a[2], b[0] - b[2]));
}

bool HasDiagonalArtifactInner(
    const MsdfArtifactClassifier& classifier,
    float am,
    float dm,
    const std::array<float, 3>& a,
    const std::array<float, 3>& l,
    const std::array<float, 3>& q,
    float dA,
    float dBC,
    float dD,
    float tEx0,
    float tEx1)
{
    float roots[2]{};
    const int rootCount = SolveQuadratic(roots, dD - dBC + dA, dBC - dA - dA, dA);
    for (int index = 0; index < rootCount; ++index)
    {
        const float t = roots[index];
        if (t <= 0.01f || t >= 0.99f)
        {
            continue;
        }

        const float xm = InterpolatedMedian(a, l, q, t);
        int rangeFlags = classifier.rangeTest(0.0f, 1.0f, t, am, dm, xm);
        if (tEx0 > 0.0f && tEx0 < 1.0f)
        {
            float tEnd[2]{ 0.0f, 1.0f };
            float em[2]{ am, dm };
            const int slot = tEx0 > t ? 1 : 0;
            tEnd[slot] = tEx0;
            em[slot] = InterpolatedMedian(a, l, q, tEx0);
            rangeFlags |= classifier.rangeTest(tEnd[0], tEnd[1], t, em[0], em[1], xm);
        }
        if (tEx1 > 0.0f && tEx1 < 1.0f)
        {
            float tEnd[2]{ 0.0f, 1.0f };
            float em[2]{ am, dm };
            const int slot = tEx1 > t ? 1 : 0;
            tEnd[slot] = tEx1;
            em[slot] = InterpolatedMedian(a, l, q, tEx1);
            rangeFlags |= classifier.rangeTest(tEnd[0], tEnd[1], t, em[0], em[1], xm);
        }
        if (classifier.evaluate(t, xm, rangeFlags))
        {
            return true;
        }
    }
    return false;
}

bool HasDiagonalArtifact(
    const MsdfArtifactClassifier& classifier,
    float am,
    const std::array<float, 3>& a,
    const std::array<float, 3>& b,
    const std::array<float, 3>& c,
    const std::array<float, 3>& d)
{
    const float dm = Median(d[0], d[1], d[2]);
    if (std::abs(am - 0.5f) < std::abs(dm - 0.5f))
    {
        return false;
    }

    const std::array<float, 3> abc{
        a[0] - b[0] - c[0],
        a[1] - b[1] - c[1],
        a[2] - b[2] - c[2],
    };
    const std::array<float, 3> l{
        -a[0] - abc[0],
        -a[1] - abc[1],
        -a[2] - abc[2],
    };
    const std::array<float, 3> q{
        d[0] + abc[0],
        d[1] + abc[1],
        d[2] + abc[2],
    };
    const std::array<float, 3> tEx{
        std::abs(q[0]) > 0.000001f ? -0.5f * l[0] / q[0] : -1.0f,
        std::abs(q[1]) > 0.000001f ? -0.5f * l[1] / q[1] : -1.0f,
        std::abs(q[2]) > 0.000001f ? -0.5f * l[2] / q[2] : -1.0f,
    };
    return HasDiagonalArtifactInner(classifier, am, dm, a, l, q, a[1] - a[0], b[1] - b[0] + c[1] - c[0], d[1] - d[0], tEx[0], tEx[1]) ||
        HasDiagonalArtifactInner(classifier, am, dm, a, l, q, a[2] - a[1], b[2] - b[1] + c[2] - c[1], d[2] - d[1], tEx[1], tEx[2]) ||
        HasDiagonalArtifactInner(classifier, am, dm, a, l, q, a[0] - a[2], b[0] - b[2] + c[0] - c[2], d[0] - d[2], tEx[2], tEx[0]);
}

bool EdgeBetweenTexelsChannel(const std::array<float, 3>& a, const std::array<float, 3>& b, std::size_t channel)
{
    const float denominator = a[channel] - b[channel];
    if (std::abs(denominator) <= 0.000001f)
    {
        return false;
    }
    const float t = (a[channel] - 0.5f) / denominator;
    if (t > 0.0f && t < 1.0f)
    {
        const std::array<float, 3> c{
            a[0] + ((b[0] - a[0]) * t),
            a[1] + ((b[1] - a[1]) * t),
            a[2] + ((b[2] - a[2]) * t),
        };
        return Median(c[0], c[1], c[2]) == c[channel];
    }
    return false;
}

std::uint32_t EdgeBetweenTexels(const std::array<float, 3>& a, const std::array<float, 3>& b)
{
    return (EdgeBetweenTexelsChannel(a, b, 0u) ? 0x1u : 0u) |
        (EdgeBetweenTexelsChannel(a, b, 1u) ? 0x2u : 0u) |
        (EdgeBetweenTexelsChannel(a, b, 2u) ? 0x4u : 0u);
}

void ProtectExtremeChannels(std::uint8_t* stencil, const std::array<float, 3>& msd, float median, std::uint32_t mask)
{
    if (((mask & 0x1u) != 0u && msd[0] != median) ||
        ((mask & 0x2u) != 0u && msd[1] != median) ||
        ((mask & 0x4u) != 0u && msd[2] != median))
    {
        *stencil |= MsdfErrorStencilProtected;
    }
}

void CorrectMsdfErrors(
    std::vector<std::array<float, 3>>* pixels,
    std::uint32_t width,
    std::uint32_t height,
    const GlyphShape& shape,
    float minX,
    float maxY)
{
    auto& data = *pixels;
    auto pixel = [&](std::uint32_t x, std::uint32_t y) -> std::array<float, 3>& {
        return data[static_cast<std::size_t>(y) * width + x];
    };
    auto stencilAt = [&](std::vector<std::uint8_t>& stencil, std::uint32_t x, std::uint32_t y) -> std::uint8_t& {
        return stencil[static_cast<std::size_t>(y) * width + x];
    };
    std::vector<int> contourWindings;
    contourWindings.reserve(shape.contourSegmentIndices.size());
    for (const std::vector<std::size_t>& contourSegments : shape.contourSegmentIndices)
    {
        contourWindings.push_back(ContourWinding(shape, contourSegments));
    }
    const MsdfErrorContext errorContext{
        .pixels = pixels,
        .shape = &shape,
        .contourWindings = &contourWindings,
        .width = width,
        .height = height,
        .minX = minX,
        .maxY = maxY,
    };

    std::vector<std::uint8_t> stencil(static_cast<std::size_t>(width) * height, 0u);
    for (const std::vector<std::size_t>& contourSegments : shape.contourSegmentIndices)
    {
        if (contourSegments.empty())
        {
            continue;
        }
        const Segment* previous = &shape.segments[contourSegments.back()];
        for (std::size_t segmentIndex : contourSegments)
        {
            const Segment& current = shape.segments[segmentIndex];
            const std::uint32_t commonColor = previous->colorMask & current.colorMask;
            if ((commonColor & (commonColor - 1u)) == 0u)
            {
                const Vec2 corner = SegmentPoint(current, 0.0f);
                const int left = static_cast<int>(std::floor(corner.x - minX + kPaddingPixels - 0.5f));
                const int bottom = static_cast<int>(std::floor(maxY - corner.y + kPaddingPixels - 0.5f));
                const int right = left + 1;
                const int top = bottom + 1;
                const std::array<std::pair<int, int>, 4> protectedTexels{
                    std::pair<int, int>{ left, bottom },
                    std::pair<int, int>{ right, bottom },
                    std::pair<int, int>{ left, top },
                    std::pair<int, int>{ right, top },
                };
                for (const auto& texel : protectedTexels)
                {
                    if (texel.first >= 0 && texel.second >= 0 &&
                        texel.first < static_cast<int>(width) && texel.second < static_cast<int>(height))
                    {
                        stencilAt(stencil, static_cast<std::uint32_t>(texel.first), static_cast<std::uint32_t>(texel.second)) |= MsdfErrorStencilProtected;
                    }
                }
            }
            previous = &current;
        }
    }

    const float protectionRadius = 1.001f / (2.0f * std::max(kDistanceRangePixels, 0.0001f));
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x + 1u < width; ++x)
        {
            const std::array<float, 3>& left = pixel(x, y);
            const std::array<float, 3>& right = pixel(x + 1u, y);
            const float leftMedian = Median(left[0], left[1], left[2]);
            const float rightMedian = Median(right[0], right[1], right[2]);
            if (std::abs(leftMedian - 0.5f) + std::abs(rightMedian - 0.5f) < protectionRadius)
            {
                const std::uint32_t mask = EdgeBetweenTexels(left, right);
                ProtectExtremeChannels(&stencilAt(stencil, x, y), left, leftMedian, mask);
                ProtectExtremeChannels(&stencilAt(stencil, x + 1u, y), right, rightMedian, mask);
            }
        }
    }
    for (std::uint32_t y = 0; y + 1u < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::array<float, 3>& bottom = pixel(x, y);
            const std::array<float, 3>& top = pixel(x, y + 1u);
            const float bottomMedian = Median(bottom[0], bottom[1], bottom[2]);
            const float topMedian = Median(top[0], top[1], top[2]);
            if (std::abs(bottomMedian - 0.5f) + std::abs(topMedian - 0.5f) < protectionRadius)
            {
                const std::uint32_t mask = EdgeBetweenTexels(bottom, top);
                ProtectExtremeChannels(&stencilAt(stencil, x, y), bottom, bottomMedian, mask);
                ProtectExtremeChannels(&stencilAt(stencil, x, y + 1u), top, topMedian, mask);
            }
        }
    }
    for (std::uint32_t y = 0; y + 1u < height; ++y)
    {
        for (std::uint32_t x = 0; x + 1u < width; ++x)
        {
            const std::array<float, 3>& lb = pixel(x, y);
            const std::array<float, 3>& rb = pixel(x + 1u, y);
            const std::array<float, 3>& lt = pixel(x, y + 1u);
            const std::array<float, 3>& rt = pixel(x + 1u, y + 1u);
            const float lbMedian = Median(lb[0], lb[1], lb[2]);
            const float rbMedian = Median(rb[0], rb[1], rb[2]);
            const float ltMedian = Median(lt[0], lt[1], lt[2]);
            const float rtMedian = Median(rt[0], rt[1], rt[2]);
            const float diagonalProtectionRadius = protectionRadius * std::sqrt(2.0f);
            if (std::abs(lbMedian - 0.5f) + std::abs(rtMedian - 0.5f) < diagonalProtectionRadius)
            {
                const std::uint32_t mask = EdgeBetweenTexels(lb, rt);
                ProtectExtremeChannels(&stencilAt(stencil, x, y), lb, lbMedian, mask);
                ProtectExtremeChannels(&stencilAt(stencil, x + 1u, y + 1u), rt, rtMedian, mask);
            }
            if (std::abs(rbMedian - 0.5f) + std::abs(ltMedian - 0.5f) < diagonalProtectionRadius)
            {
                const std::uint32_t mask = EdgeBetweenTexels(rb, lt);
                ProtectExtremeChannels(&stencilAt(stencil, x + 1u, y), rb, rbMedian, mask);
                ProtectExtremeChannels(&stencilAt(stencil, x, y + 1u), lt, ltMedian, mask);
            }
        }
    }

    const float artifactSpan = 1.11111111111111111f / (2.0f * std::max(kDistanceRangePixels, 0.0001f));
    const float diagonalArtifactSpan = artifactSpan * std::sqrt(2.0f);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::array<float, 3>& current = pixel(x, y);
            const float currentMedian = Median(current[0], current[1], current[2]);
            const bool protectedFlag = (stencilAt(stencil, x, y) & MsdfErrorStencilProtected) != 0u;
            const Vec2 sdfCoord{ static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f };
            auto classifier = [&](float span, Vec2 direction) {
                MsdfArtifactClassifier result{};
                result.context = &errorContext;
                result.msd = &current;
                result.sdfCoord = sdfCoord;
                result.direction = direction;
                result.span = span;
                result.protectedFlag = protectedFlag;
                return result;
            };
            bool error = false;
            if (x > 0u)
            {
                error = error || HasLinearArtifact(classifier(artifactSpan, { -1.0f, 0.0f }), currentMedian, current, pixel(x - 1u, y));
            }
            if (y > 0u)
            {
                error = error || HasLinearArtifact(classifier(artifactSpan, { 0.0f, -1.0f }), currentMedian, current, pixel(x, y - 1u));
            }
            if (x + 1u < width)
            {
                error = error || HasLinearArtifact(classifier(artifactSpan, { 1.0f, 0.0f }), currentMedian, current, pixel(x + 1u, y));
            }
            if (y + 1u < height)
            {
                error = error || HasLinearArtifact(classifier(artifactSpan, { 0.0f, 1.0f }), currentMedian, current, pixel(x, y + 1u));
            }
            if (x > 0u && y > 0u)
            {
                error = error || HasDiagonalArtifact(classifier(diagonalArtifactSpan, { -1.0f, -1.0f }), currentMedian, current, pixel(x - 1u, y), pixel(x, y - 1u), pixel(x - 1u, y - 1u));
            }
            if (x + 1u < width && y > 0u)
            {
                error = error || HasDiagonalArtifact(classifier(diagonalArtifactSpan, { 1.0f, -1.0f }), currentMedian, current, pixel(x + 1u, y), pixel(x, y - 1u), pixel(x + 1u, y - 1u));
            }
            if (x > 0u && y + 1u < height)
            {
                error = error || HasDiagonalArtifact(classifier(diagonalArtifactSpan, { -1.0f, 1.0f }), currentMedian, current, pixel(x - 1u, y), pixel(x, y + 1u), pixel(x - 1u, y + 1u));
            }
            if (x + 1u < width && y + 1u < height)
            {
                error = error || HasDiagonalArtifact(classifier(diagonalArtifactSpan, { 1.0f, 1.0f }), currentMedian, current, pixel(x + 1u, y), pixel(x, y + 1u), pixel(x + 1u, y + 1u));
            }
            if (error)
            {
                stencilAt(stencil, x, y) |= MsdfErrorStencilError;
            }
        }
    }

    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            if ((stencilAt(stencil, x, y) & MsdfErrorStencilError) != 0u)
            {
                std::array<float, 3>& current = pixel(x, y);
                const float median = Median(current[0], current[1], current[2]);
                current = { median, median, median };
            }
        }
    }
}

void RenderGlyphMsdf(
    const GlyphShape& shape,
    float minX,
    float maxY,
    std::uint32_t atlasX,
    std::uint32_t atlasY,
    std::uint32_t width,
    std::uint32_t height,
    std::vector<std::byte>* atlasPixels)
{
    std::vector<std::array<float, 3>> msdfPixels(static_cast<std::size_t>(width) * height);
    std::vector<int> contourWindings;
    contourWindings.reserve(shape.contourSegmentIndices.size());
    for (const std::vector<std::size_t>& contourSegments : shape.contourSegmentIndices)
    {
        contourWindings.push_back(ContourWinding(shape, contourSegments));
    }

    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const Vec2 point{
                minX + (static_cast<float>(x) + 0.5f - kPaddingPixels),
                maxY - (static_cast<float>(y) + 0.5f - kPaddingPixels),
            };
            std::vector<std::array<EdgeDistance, 3>> contourSelectors(shape.contourSegmentIndices.size());
            SignedDistance fallbackDistance{
                -std::numeric_limits<float>::max(),
                0.0f,
            };

            for (std::size_t contourIndex = 0; contourIndex < shape.contourSegmentIndices.size(); ++contourIndex)
            {
                const std::vector<std::size_t>& contourSegments = shape.contourSegmentIndices[contourIndex];
                if (contourSegments.empty())
                {
                    continue;
                }
                for (std::size_t offset = 0; offset < contourSegments.size(); ++offset)
                {
                    const Segment& segment = shape.segments[contourSegments[offset]];
                    const Segment& previous = shape.segments[contourSegments[(offset + contourSegments.size() - 1u) % contourSegments.size()]];
                    const Segment& next = shape.segments[contourSegments[(offset + 1u) % contourSegments.size()]];
                    float param = 0.0f;
                    const SignedDistance signedDistance = SignedDistanceToSegment(point, segment, &param);
                    if (signedDistance < fallbackDistance)
                    {
                        fallbackDistance = signedDistance;
                    }
                    for (std::size_t channel = 0; channel < 3u; ++channel)
                    {
                        if ((segment.colorMask & (1u << channel)) != 0u)
                        {
                            AddEdgeDistance(&contourSelectors[contourIndex][channel], point, previous, segment, next, signedDistance, param);
                        }
                    }
                }
            }

            std::array<float, 3>& msdf = msdfPixels[static_cast<std::size_t>(y) * width + x];
            const std::array<float, 3> distance = ResolveOverlappingContourDistance(
                contourSelectors,
                contourWindings,
                point,
                fallbackDistance);
            for (std::size_t channel = 0; channel < 3u; ++channel)
            {
                msdf[channel] = std::clamp(0.5f + (distance[channel] / (2.0f * kDistanceRangePixels)), 0.0f, 1.0f);
            }
        }
    }

    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const Vec2 point{
                minX + (static_cast<float>(x) + 0.5f - kPaddingPixels),
                maxY - (static_cast<float>(y) + 0.5f - kPaddingPixels),
            };
            std::array<float, 3>& msdf = msdfPixels[static_cast<std::size_t>(y) * width + x];
            const bool inside = PointInShape(point, shape);
            const bool medianInside = Median(msdf[0], msdf[1], msdf[2]) > 0.5f;
            if (inside != medianInside)
            {
                for (float& channel : msdf)
                {
                    channel = 1.0f - channel;
                }
            }
        }
    }

    CorrectMsdfErrors(&msdfPixels, width, height, shape, minX, maxY);

    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::array<float, 3>& msdf = msdfPixels[static_cast<std::size_t>(y) * width + x];
            const std::size_t outputIndex = (static_cast<std::size_t>(atlasY + y) * kAtlasSize + (atlasX + x)) * 4u;
            for (std::size_t channel = 0; channel < 3u; ++channel)
            {
                (*atlasPixels)[outputIndex + channel] = std::byte{
                    static_cast<std::uint8_t>(std::clamp(msdf[channel] * 255.0f, 0.0f, 255.0f))
                };
            }
            (*atlasPixels)[outputIndex + 3u] = std::byte{ 0xFF };
        }
    }
}

std::filesystem::path ResolveFontPath(const ConverterConfig& config)
{
    if (std::filesystem::is_regular_file(config.sourceRoot))
    {
        return config.sourceRoot;
    }
    return config.sourceRoot / "Roboto-VariableFont_wdth,wght.ttf";
}

} // namespace

bool GenerateFontMsdfAtlas(const ConverterConfig& config, ImportedPack* outPack, std::string* error)
{
    const std::filesystem::path fontPath = ResolveFontPath(config);
    if (!std::filesystem::exists(fontPath))
    {
        *error = "font source file does not exist: " + fontPath.string();
        return false;
    }

    std::cout << "[font] Loading " << fontPath.string() << '\n';

    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0)
    {
        *error = "FT_Init_FreeType failed";
        return false;
    }

    FT_Face face = nullptr;
    if (FT_New_Face(library, fontPath.string().c_str(), 0, &face) != 0)
    {
        FT_Done_FreeType(library);
        *error = "FT_New_Face failed for: " + fontPath.string();
        return false;
    }

    if (FT_Set_Pixel_Sizes(face, 0, kPixelSize) != 0)
    {
        FT_Done_Face(face);
        FT_Done_FreeType(library);
        *error = "FT_Set_Pixel_Sizes failed";
        return false;
    }

    std::vector<std::byte> atlasPixels(static_cast<std::size_t>(kAtlasSize) * kAtlasSize * 4u, std::byte{ 0x80 });
    for (std::size_t index = 3u; index < atlasPixels.size(); index += 4u)
    {
        atlasPixels[index] = std::byte{ 0xFF };
    }

    std::vector<RuntimeAssets::FontGlyphRecord> glyphs;
    std::uint32_t penX = 0;
    std::uint32_t penY = 0;
    std::uint32_t rowHeight = 0;
    std::cout << "[font] Rasterizing codepoints " << kFirstCodepoint
              << "-" << kLastCodepoint << " into " << kAtlasSize
              << "x" << kAtlasSize << " atlas\n";

    for (std::uint32_t codepoint = kFirstCodepoint; codepoint <= kLastCodepoint; ++codepoint)
    {
        if ((codepoint - kFirstCodepoint) % 16u == 0u)
        {
            std::cout << "[font]     Codepoint " << codepoint << '\n';
        }

        if (FT_Load_Char(face, codepoint, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) != 0)
        {
            continue;
        }

        FT_GlyphSlot glyphSlot = face->glyph;
        FT_BBox bbox{};
        FT_Outline_Get_CBox(&glyphSlot->outline, &bbox);

        const float minX = FromFt26Dot6(bbox.xMin);
        const float minY = FromFt26Dot6(bbox.yMin);
        const float maxX = FromFt26Dot6(bbox.xMax);
        const float maxY = FromFt26Dot6(bbox.yMax);
        const std::uint32_t glyphWidth = std::max<std::uint32_t>(
            static_cast<std::uint32_t>(std::ceil(std::max(maxX - minX, 0.0f) + (kPaddingPixels * 2.0f))),
            1u);
        const std::uint32_t glyphHeight = std::max<std::uint32_t>(
            static_cast<std::uint32_t>(std::ceil(std::max(maxY - minY, 0.0f) + (kPaddingPixels * 2.0f))),
            1u);

        if (penX + glyphWidth > kAtlasSize)
        {
            penX = 0;
            penY += rowHeight;
            rowHeight = 0;
        }
        if (penY + glyphHeight > kAtlasSize)
        {
            FT_Done_Face(face);
            FT_Done_FreeType(library);
            *error = "font MSDF atlas is too small for the requested glyph range";
            return false;
        }

        RuntimeAssets::FontGlyphRecord entry{};
        entry.codepoint = codepoint;
        entry.atlasX = penX;
        entry.atlasY = penY;
        entry.atlasWidth = glyphWidth;
        entry.atlasHeight = glyphHeight;
        entry.planeLeft = minX - kPaddingPixels;
        entry.planeBottom = minY - kPaddingPixels;
        entry.planeRight = maxX + kPaddingPixels;
        entry.planeTop = maxY + kPaddingPixels;
        entry.advance = FromFt26Dot6(glyphSlot->advance.x);

        if (glyphSlot->outline.n_points > 0)
        {
            GlyphShape shape;
            if (!BuildShape(glyphSlot->outline, &shape, error))
            {
                FT_Done_Face(face);
                FT_Done_FreeType(library);
                return false;
            }
            RenderGlyphMsdf(shape, minX, maxY, penX, penY, glyphWidth, glyphHeight, &atlasPixels);
        }

        glyphs.push_back(entry);
        penX += glyphWidth;
        rowHeight = std::max(rowHeight, glyphHeight);
    }

    FT_Done_Face(face);
    FT_Done_FreeType(library);

    ImportedTexture atlasTexture{};
    atlasTexture.name = config.packName + "_msdf_atlas";
    atlasTexture.normalizedBasename = atlasTexture.name;
    atlasTexture.sourcePath = fontPath.generic_string();
    atlasTexture.width = kAtlasSize;
    atlasTexture.height = kAtlasSize;
    atlasTexture.layerCount = 1;
    atlasTexture.mipCount = 1;
    atlasTexture.format = RuntimeAssets::TextureFormat::RGBA8_UNORM;
    atlasTexture.dimension = RuntimeAssets::TextureDimension::Texture2D;
    atlasTexture.flags = RuntimeAssets::TextureFlagNone;
    atlasTexture.payload = std::move(atlasPixels);

    const std::uint32_t atlasTextureIndex = static_cast<std::uint32_t>(outPack->textures.size());
    outPack->textures.push_back(std::move(atlasTexture));

    ImportedFontAtlas fontAtlas{};
    fontAtlas.name = config.packName;
    fontAtlas.record.textureIndex = atlasTextureIndex;
    fontAtlas.record.firstCodepoint = kFirstCodepoint;
    fontAtlas.record.lastCodepoint = kLastCodepoint;
    fontAtlas.record.pixelSize = static_cast<float>(kPixelSize);
    fontAtlas.record.distanceRange = kDistanceRangePixels;
    fontAtlas.glyphs = std::move(glyphs);
    outPack->fontAtlases.push_back(std::move(fontAtlas));
    std::cout << "[font] Generated " << outPack->fontAtlases.back().glyphs.size()
              << " glyph record(s)\n";
    return true;
}
