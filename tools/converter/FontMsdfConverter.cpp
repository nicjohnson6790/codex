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

struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;
};

struct Segment
{
    Vec2 a;
    Vec2 b;
    std::uint32_t channel = 0;
};

struct GlyphShape
{
    std::vector<Segment> segments;
    std::vector<std::vector<Vec2>> contours;
};

struct OutlineBuildContext
{
    GlyphShape shape;
    std::vector<Vec2> currentContour;
    Vec2 current{};
    Vec2 contourStart{};
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

void AddSegment(OutlineBuildContext* context, Vec2 a, Vec2 b)
{
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    if ((dx * dx) + (dy * dy) < 0.0001f)
    {
        return;
    }

    Segment segment{};
    segment.a = a;
    segment.b = b;
    segment.channel = static_cast<std::uint32_t>(context->shape.segments.size() % 3u);
    context->shape.segments.push_back(segment);
    context->currentContour.push_back(b);
}

int MoveToCallback(const FT_Vector* to, void* user)
{
    auto* context = static_cast<OutlineBuildContext*>(user);
    if (context->currentContour.size() > 1u)
    {
        context->shape.contours.push_back(context->currentContour);
    }
    context->current = FromFtVector(to);
    context->contourStart = context->current;
    context->currentContour.clear();
    context->currentContour.push_back(context->current);
    return 0;
}

int LineToCallback(const FT_Vector* to, void* user)
{
    auto* context = static_cast<OutlineBuildContext*>(user);
    const Vec2 destination = FromFtVector(to);
    AddSegment(context, context->current, destination);
    context->current = destination;
    return 0;
}

int ConicToCallback(const FT_Vector* control, const FT_Vector* to, void* user)
{
    auto* context = static_cast<OutlineBuildContext*>(user);
    const Vec2 start = context->current;
    const Vec2 controlPoint = FromFtVector(control);
    const Vec2 destination = FromFtVector(to);
    constexpr int kSteps = 16;
    Vec2 previous = start;
    for (int step = 1; step <= kSteps; ++step)
    {
        const float t = static_cast<float>(step) / static_cast<float>(kSteps);
        const Vec2 point = Quadratic(start, controlPoint, destination, t);
        AddSegment(context, previous, point);
        previous = point;
    }
    context->current = destination;
    return 0;
}

int CubicToCallback(const FT_Vector* control1, const FT_Vector* control2, const FT_Vector* to, void* user)
{
    auto* context = static_cast<OutlineBuildContext*>(user);
    const Vec2 start = context->current;
    const Vec2 controlPoint1 = FromFtVector(control1);
    const Vec2 controlPoint2 = FromFtVector(control2);
    const Vec2 destination = FromFtVector(to);
    constexpr int kSteps = 24;
    Vec2 previous = start;
    for (int step = 1; step <= kSteps; ++step)
    {
        const float t = static_cast<float>(step) / static_cast<float>(kSteps);
        const Vec2 point = Cubic(start, controlPoint1, controlPoint2, destination, t);
        AddSegment(context, previous, point);
        previous = point;
    }
    context->current = destination;
    return 0;
}

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
    if (context.currentContour.size() > 1u)
    {
        context.shape.contours.push_back(context.currentContour);
    }

    *outShape = std::move(context.shape);
    return true;
}

float DistanceToSegment(Vec2 point, const Segment& segment)
{
    const float vx = segment.b.x - segment.a.x;
    const float vy = segment.b.y - segment.a.y;
    const float wx = point.x - segment.a.x;
    const float wy = point.y - segment.a.y;
    const float lengthSquared = (vx * vx) + (vy * vy);
    const float t = lengthSquared > 0.0f ? std::clamp(((wx * vx) + (wy * vy)) / lengthSquared, 0.0f, 1.0f) : 0.0f;
    const float px = segment.a.x + (vx * t);
    const float py = segment.a.y + (vy * t);
    const float dx = point.x - px;
    const float dy = point.y - py;
    return std::sqrt((dx * dx) + (dy * dy));
}

bool PointInShape(Vec2 point, const GlyphShape& shape)
{
    bool inside = false;
    for (const std::vector<Vec2>& contour : shape.contours)
    {
        if (contour.size() < 3u)
        {
            continue;
        }

        for (std::size_t i = 0, j = contour.size() - 1u; i < contour.size(); j = i++)
        {
            const Vec2 a = contour[i];
            const Vec2 b = contour[j];
            const bool crosses = ((a.y > point.y) != (b.y > point.y)) &&
                (point.x < ((b.x - a.x) * (point.y - a.y) / ((b.y - a.y) + 0.000001f)) + a.x);
            if (crosses)
            {
                inside = !inside;
            }
        }
    }
    return inside;
}

std::uint8_t EncodeDistance(float signedDistance)
{
    const float normalized = 0.5f + (signedDistance / (2.0f * kDistanceRangePixels));
    return static_cast<std::uint8_t>(std::clamp(normalized * 255.0f, 0.0f, 255.0f));
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
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const Vec2 point{
                minX + (static_cast<float>(x) + 0.5f - kPaddingPixels),
                maxY - (static_cast<float>(y) + 0.5f - kPaddingPixels),
            };
            const bool inside = PointInShape(point, shape);
            std::array<float, 3> distances{
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(),
            };
            float fallbackDistance = std::numeric_limits<float>::max();

            for (const Segment& segment : shape.segments)
            {
                const float distance = DistanceToSegment(point, segment);
                fallbackDistance = std::min(fallbackDistance, distance);
                distances[segment.channel] = std::min(distances[segment.channel], distance);
            }

            const float sign = inside ? 1.0f : -1.0f;
            const std::size_t outputIndex = (static_cast<std::size_t>(atlasY + y) * kAtlasSize + (atlasX + x)) * 4u;
            for (std::size_t channel = 0; channel < 3u; ++channel)
            {
                const float distance = std::isfinite(distances[channel]) ? distances[channel] : fallbackDistance;
                (*atlasPixels)[outputIndex + channel] = std::byte{ EncodeDistance(distance * sign) };
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

        GlyphShape shape;
        if (glyphSlot->outline.n_points > 0)
        {
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
