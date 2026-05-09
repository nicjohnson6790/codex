#version 450

layout(set=3, binding=0) uniform FoliageImposterMaterialUniforms
{
    vec4 sunDirectionIntensity;
    vec4 sunColorAmbient;
} foliageMaterial;

layout(set=2, binding=0) uniform sampler2DArray imposterColorTextureArray;
layout(set=2, binding=1) uniform sampler2DArray imposterNormalTextureArray;

layout(location = 0) in vec2 fragUv0;
layout(location = 1) flat in uint fragLayerIndex0;
layout(location = 2) flat in uint fragLayerIndex1;
layout(location = 3) in float fragYawBlend;
layout(location = 4) in vec3 fragCaptureRight;
layout(location = 5) in vec3 fragCaptureUp;
layout(location = 6) in vec3 fragCaptureForward;
layout(location = 7) in vec3 fragViewDirection;

layout(location = 0) out vec4 outColor;

const float kFoliageAmbientBoost = 3.35;
const float kFoliageSkyFillStrength = 0.52;
const float kFoliageLightingScale = 1.18;

float saturate(float value)
{
    return clamp(value, 0.0, 1.0);
}

void main()
{
    vec4 colorSample0 = texture(imposterColorTextureArray, vec3(fragUv0, float(fragLayerIndex0)));
    vec4 colorSample1 = texture(imposterColorTextureArray, vec3(fragUv0, float(fragLayerIndex1)));
    vec4 colorSample = mix(colorSample0, colorSample1, fragYawBlend);
    const float alphaCutoff = 0.5;
    if (colorSample.a < alphaCutoff)
    {
        discard;
    }

    vec3 encodedNormal0 = texture(imposterNormalTextureArray, vec3(fragUv0, float(fragLayerIndex0))).xyz * 2.0 - 1.0;
    vec3 encodedNormal1 = texture(imposterNormalTextureArray, vec3(fragUv0, float(fragLayerIndex1))).xyz * 2.0 - 1.0;
    vec3 encodedNormal = normalize(mix(encodedNormal0, encodedNormal1, fragYawBlend));
    vec3 worldNormal = normalize(
        (fragCaptureRight * encodedNormal.x) +
        (fragCaptureUp * encodedNormal.y) +
        (fragCaptureForward * encodedNormal.z));

    vec3 sunDirection = normalize(foliageMaterial.sunDirectionIntensity.xyz);
    vec3 viewDirection = normalize(fragViewDirection);
    float nDotL = saturate(dot(worldNormal, sunDirection));
    float nDotV = saturate(dot(worldNormal, viewDirection));
    float backScatter = pow(saturate(dot(-viewDirection, sunDirection)), 2.0) * saturate(dot(-worldNormal, sunDirection));
    float sunIntensity = foliageMaterial.sunDirectionIntensity.w;
    vec3 sunRadiance = foliageMaterial.sunColorAmbient.rgb * sunIntensity;
    vec3 ambient = colorSample.rgb * foliageMaterial.sunColorAmbient.a * kFoliageAmbientBoost;
    vec3 skyFill = colorSample.rgb * foliageMaterial.sunColorAmbient.a * kFoliageSkyFillStrength;
    vec3 direct = colorSample.rgb * sunRadiance * (0.42 + (nDotL * 0.58));
    vec3 transmission = colorSample.rgb * sunRadiance * (0.18 * backScatter);
    vec3 fresnel = vec3(0.018) * pow(1.0 - nDotV, 5.0) * sunIntensity;
    vec3 litColor = (ambient + skyFill + direct + transmission + fresnel) * kFoliageLightingScale;

    outColor = vec4(litColor, colorSample.a);
}
