#version 450
#extension GL_GOOGLE_include_directive : require

layout(set=2, binding=0) uniform sampler2DArray imposterColorTextureArray;
#include "foliage_imposter_blend.glsl"

layout(location = 10) flat in uvec2 fragUpperLayers;
layout(location = 11) in vec2 fragUv1;
layout(location = 12) flat in float fragPitchBlend;
layout(location = 0) in vec2 fragUv0;
layout(location = 1) flat in uint fragLayerIndex0;
layout(location = 2) flat in uint fragLayerIndex1;
layout(location = 3) in float fragYawBlend;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 normalWeights;
    vec4 colorSample=imposterBlendedColor(imposterColorTextureArray,fragUv0,fragUv1,
        uvec2(fragLayerIndex0,fragLayerIndex1),fragUpperLayers,fragYawBlend,fragPitchBlend,normalWeights);
    if (colorSample.a < 0.5)
    {
        discard;
    }

    outColor = vec4(0.0);
}
