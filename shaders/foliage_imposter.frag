#version 450
#extension GL_GOOGLE_include_directive : require
#define FOLIAGE_CLOUD_BINDING 2
#define FOLIAGE_NOISE_BINDING 3
#define SKY_PROBE_COEFFICIENT_BINDING 4
#define SKY_PROBE_REGION_BINDING 5
#include "foliage_lighting.glsl"


layout(early_fragment_tests) in;

layout(set=3, binding=0) uniform FoliageImposterMaterialUniforms
{
    vec4 sunDirectionIntensity;
    vec4 sunColorAmbient;
} foliageMaterial;

layout(set=2, binding=0) uniform sampler2DArray imposterColorTextureArray;
#include "foliage_imposter_blend.glsl"
layout(set=2, binding=1) uniform sampler2DArray imposterNormalTextureArray;

layout(location = 10) flat in uvec2 fragUpperLayers;
layout(location = 11) in vec2 fragUv1;
layout(location = 12) flat in float fragPitchBlend;
layout(location = 0) in vec2 fragUv0;
layout(location = 1) flat in uint fragLayerIndex0;
layout(location = 2) flat in uint fragLayerIndex1;
layout(location = 3) in float fragYawBlend;
layout(location = 4) in vec3 fragTreeRight;
layout(location = 5) in vec3 fragTreeUp;
layout(location = 6) in vec3 fragTreeForward;
layout(location = 7) in vec3 fragViewDirection;
layout(location = 8) in vec3 fragPosition;
layout(location = 9) flat in uint fragIlluminationRegion;

layout(location = 0) out vec4 outColor;

float saturate(float value)
{
    return clamp(value, 0.0, 1.0);
}

void main()
{
    vec4 normalWeights;
    vec4 colorSample=imposterBlendedColor(imposterColorTextureArray,fragUv0,fragUv1,
        uvec2(fragLayerIndex0,fragLayerIndex1),fragUpperLayers,fragYawBlend,fragPitchBlend,normalWeights);
    vec2 lowDx=dFdx(fragUv0),lowDy=dFdy(fragUv0);
    vec2 highDx=dFdx(fragUv1),highDy=dFdy(fragUv1);
    const float alphaCutoff = 0.5;
    if (colorSample.a < alphaCutoff)
    {
        discard;
    }

    vec3 n0=textureGrad(imposterNormalTextureArray,vec3(fragUv0,float(fragLayerIndex0)),lowDx,lowDy).rgb*2-1;
    vec3 n1=textureGrad(imposterNormalTextureArray,vec3(fragUv0,float(fragLayerIndex1)),lowDx,lowDy).rgb*2-1;
    vec3 n2=textureGrad(imposterNormalTextureArray,vec3(fragUv1,float(fragUpperLayers.x)),highDx,highDy).rgb*2-1;
    vec3 n3=textureGrad(imposterNormalTextureArray,vec3(fragUv1,float(fragUpperLayers.y)),highDx,highDy).rgb*2-1;
    vec3 localNormal=n0*normalWeights.x+n1*normalWeights.y+n2*normalWeights.z+n3*normalWeights.w;
    vec3 mixedNormal=fragTreeRight*localNormal.x+fragTreeUp*localNormal.y+fragTreeForward*localNormal.z;
    vec3 worldNormal = dot(mixedNormal,mixedNormal)>1e-8 ? normalize(mixedNormal) : normalize(fragViewDirection);

    vec3 sunDirection = normalize(foliageMaterial.sunDirectionIntensity.xyz);
    vec3 viewDirection = normalize(fragViewDirection);
    float nDotL = saturate(dot(worldNormal, sunDirection));
    float nDotV = saturate(dot(worldNormal, viewDirection));
    float backScatter = pow(saturate(dot(-viewDirection, sunDirection)), 2.0) * saturate(dot(-worldNormal, sunDirection));
    vec3 sunRadiance = foliageSun(fragPosition,sunDirection);
    vec3 ambient = colorSample.rgb / 3.14159265359 * foliageSky(fragPosition,worldNormal,fragIlluminationRegion);
    vec3 direct = colorSample.rgb * sunRadiance * (nDotL / 3.14159265359 + 0.18 * backScatter);
    vec3 litColor = ambient + direct;

    outColor = vec4(litColor, colorSample.a);
}
