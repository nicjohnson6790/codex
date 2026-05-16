#version 450

layout(set=2, binding=0) uniform sampler2D fontAtlas;

layout(set=3, binding=0) uniform WorldTextMaterial
{
    vec4 params;
} material;

struct TextGroup
{
    vec4 origin;
    vec4 right;
    vec4 up;
    vec4 drawBounds;
    uvec4 glyphRangeAndStyle;
};

struct GlyphInstance
{
    vec4 offsetAndGlyph;
};

struct GlyphMetric
{
    vec4 uvRect;
    vec4 planeBounds;
};

struct TextStyle
{
    vec4 baseColor;
    vec4 strokeColor;
    vec4 glowColor;
    vec4 widths;
    vec4 glowOffset;
};

layout(set=2, binding=1, std430) readonly buffer TextGroupBuffer
{
    TextGroup groups[];
} textGroupBuffer;

layout(set=2, binding=2, std430) readonly buffer GlyphInstanceBuffer
{
    GlyphInstance glyphs[];
} glyphInstanceBuffer;

layout(set=2, binding=3, std430) readonly buffer GlyphMetricBuffer
{
    GlyphMetric metrics[];
} glyphMetricBuffer;

layout(set=2, binding=4, std430) readonly buffer TextStyleBuffer
{
    TextStyle styles[];
} textStyleBuffer;

layout(location = 0) in vec2 fragLocalFontPosition;
layout(location = 1) flat in uint fragGroupIndex;

layout(location = 0) out vec4 outColor;

float median(float r, float g, float b)
{
    return max(min(r, g), min(max(r, g), b));
}

float atlasDistance(vec2 uv)
{
    vec3 msd = texture(fontAtlas, uv).rgb;
    return median(msd.r, msd.g, msd.b);
}

float screenPxRange(vec2 uv)
{
    vec2 unitRange = vec2(material.params.x) / vec2(textureSize(fontAtlas, 0));
    vec2 dtx = dFdx(uv);
    vec2 dty = dFdy(uv);
    vec2 screenTexSize = inversesqrt((dtx * dtx) + (dty * dty));
    return max(0.5 * dot(unitRange, screenTexSize), 1.0);
}

vec3 sampleGlyphDistance(uint glyphBufferIndex, vec2 localFontPosition)
{
    GlyphInstance instance = glyphInstanceBuffer.glyphs[glyphBufferIndex];
    GlyphMetric metric = glyphMetricBuffer.metrics[uint(instance.offsetAndGlyph.z + 0.5)];
    vec2 glyphLocal = localFontPosition - instance.offsetAndGlyph.xy;
    vec2 boundsMin = metric.planeBounds.xy;
    vec2 boundsMax = metric.planeBounds.zw;
    vec2 atlasSize = vec2(textureSize(fontAtlas, 0));
    vec2 glyphTexelCount = max(abs(metric.uvRect.zw - metric.uvRect.xy) * atlasSize, vec2(1.0));
    vec2 fontUnitsPerTexel = (boundsMax - boundsMin) / glyphTexelCount;
    vec2 sampleInset = fontUnitsPerTexel * 0.5;
    vec2 sampleBoundsMin = boundsMin + sampleInset;
    vec2 sampleBoundsMax = boundsMax - sampleInset;

    if (glyphLocal.x < sampleBoundsMin.x || glyphLocal.x > sampleBoundsMax.x ||
        glyphLocal.y < sampleBoundsMin.y || glyphLocal.y > sampleBoundsMax.y)
    {
        return vec3(-1.0e20, 1.0, -1.0);
    }

    vec2 uv = vec2(
        mix(metric.uvRect.x, metric.uvRect.z, (glyphLocal.x - boundsMin.x) / max(boundsMax.x - boundsMin.x, 0.0001)),
        mix(metric.uvRect.w, metric.uvRect.y, (glyphLocal.y - boundsMin.y) / max(boundsMax.y - boundsMin.y, 0.0001)));
    vec2 atlasInset = vec2(0.5) / atlasSize;
    uv = clamp(uv, metric.uvRect.xy + atlasInset, metric.uvRect.zw - atlasInset);
    float distance = atlasDistance(uv);
    if (distance <= 0.001)
    {
        return vec3(-1.0e20, 1.0, -1.0);
    }
    return vec3(distance, screenPxRange(uv), 1.0);
}

bool isValidDistanceSample(vec3 sampleValue)
{
    return sampleValue.z > 0.0;
}

vec3 unionDistanceSample(vec3 lhs, vec3 rhs)
{
    if (!isValidDistanceSample(lhs))
    {
        return rhs;
    }
    if (!isValidDistanceSample(rhs))
    {
        return lhs;
    }
    return rhs.x > lhs.x ? rhs : lhs;
}

vec3 sampleGroupDistance(TextGroup group, vec2 localFontPosition)
{
    vec3 distanceSample = vec3(-1.0e20, 1.0, -1.0);
    uint glyphStart = group.glyphRangeAndStyle.x;
    uint glyphCount = group.glyphRangeAndStyle.y;
    for (uint glyphOffset = 0u; glyphOffset < glyphCount; ++glyphOffset)
    {
        distanceSample = unionDistanceSample(
            distanceSample,
            sampleGlyphDistance(glyphStart + glyphOffset, localFontPosition));
    }
    return distanceSample;
}

float coverageFromDistance(vec3 distanceSample, float fontPixelOffset)
{
    if (!isValidDistanceSample(distanceSample))
    {
        return 0.0;
    }
    float normalizedOffset = fontPixelOffset / max(material.params.x * 2.0, 0.0001);
    float fontPixelAntialias = 1.0 / max(material.params.x * 2.0, 0.0001);
    if (distanceSample.x <= (0.5 - normalizedOffset - fontPixelAntialias))
    {
        return 0.0;
    }
    float screenPxDistance = distanceSample.y * ((distanceSample.x + normalizedOffset) - 0.5);
    return clamp(screenPxDistance + 0.5, 0.0, 1.0);
}

void main()
{
    TextGroup group = textGroupBuffer.groups[fragGroupIndex];
    TextStyle style = textStyleBuffer.styles[group.glyphRangeAndStyle.z];

    vec3 textDistance = sampleGroupDistance(group, fragLocalFontPosition);
    float baseCoverage = coverageFromDistance(textDistance, 0.0);
    float strokeCoverage = coverageFromDistance(textDistance, max(style.widths.x, 0.0));

    vec2 glowSamplePosition = fragLocalFontPosition - style.glowOffset.xy;
    vec3 glowDistance = dot(style.glowOffset.xy, style.glowOffset.xy) > 0.000001
        ? sampleGroupDistance(group, glowSamplePosition)
        : textDistance;
    float glowCoverage = coverageFromDistance(glowDistance, max(style.widths.y, 0.0));

    float strokeOnly = max(strokeCoverage - baseCoverage, 0.0);
    float glowOnly = max(glowCoverage - strokeCoverage, 0.0);

    vec4 color = vec4(0.0);
    color.rgb += style.glowColor.rgb * style.glowColor.a * glowOnly;
    color.a += style.glowColor.a * glowOnly;
    color.rgb = mix(color.rgb, style.strokeColor.rgb, strokeOnly * style.strokeColor.a);
    color.a = max(color.a, strokeOnly * style.strokeColor.a);
    color.rgb = mix(color.rgb, style.baseColor.rgb, baseCoverage * style.baseColor.a);
    color.a = max(color.a, baseCoverage * style.baseColor.a);

    if (color.a <= 0.001)
    {
        discard;
    }
    outColor = vec4(color.rgb, color.a);
}
