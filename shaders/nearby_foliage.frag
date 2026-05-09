#version 450

layout(set=3, binding=0) uniform NearbyFoliageMaterialUniforms
{
    vec4 sunDirectionIntensity;
    vec4 sunColorAmbient;
    vec4 shadingParams0;
    mat4 skyRotation;
    vec4 atmosphereParams;
    vec4 sunDirectionTimeOfDay;
} foliageMaterial;

layout(set=2, binding=0) uniform sampler2DArray baseColorTextureArray;
layout(set=2, binding=1) uniform sampler2DArray normalTextureArray;
layout(set=2, binding=2) uniform sampler2DArray roughnessTextureArray;
layout(set=2, binding=3) uniform sampler2DArray specularTextureArray;
layout(set=2, binding=4) uniform sampler2DArray aoTextureArray;
layout(set=2, binding=5) uniform sampler2DArray subsurfaceTextureArray;
layout(set=2, binding=6) uniform samplerCube skyboxTexture;
layout(set=2, binding=7) uniform sampler2DArray atmosphereLutTexture;

struct NearbyMaterialGpu
{
    uvec4 layers0;
    uvec4 layers1;
    vec4 params;
};

layout(set=2, binding=8, std430) readonly buffer NearbyMaterialBuffer
{
    NearbyMaterialGpu materials[];
} materialBuffer;

layout(location = 0) in vec2 fragUv0;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragTangent;
layout(location = 3) in vec3 fragBitangent;
layout(location = 4) in vec3 fragViewPosition;
layout(location = 5) flat in uint fragMaterialIndex;
layout(location = 6) flat in uint fragLodIndex;

layout(location = 0) out vec4 outColor;

const float kPi = 3.14159265359;
const float kInvLog256 = 0.18033688011112042;
const float kFoliageAmbientBoost = 3.35;
const float kFoliageSkyFillStrength = 0.52;
const float kFoliageLightingScale = 1.18;
const float kNearbyLod2FadeStartMeters = 105.0;
const float kNearbyLod2FadeEndMeters = 100.0;

float saturate(float value)
{
    return clamp(value, 0.0, 1.0);
}

float sunVisibility(float sunHeight)
{
    return smoothstep(-0.045, 0.02, sunHeight);
}

float daylightVisibility(float sunHeight)
{
    return smoothstep(-0.12, 0.10, sunHeight);
}

float interleavedGradientNoise(vec2 pixelCoord)
{
    return fract(52.9829189 * fract(dot(pixelCoord, vec2(0.06711056, 0.00583715))));
}

float encodeLogDistance(float distanceThroughAtmosphere)
{
    float normalizedDistance = saturate(distanceThroughAtmosphere / max(foliageMaterial.atmosphereParams.y, 0.00001));
    return log(normalizedDistance * 255.0 + 1.0) * kInvLog256;
}

vec4 sampleAtmosphere(vec3 worldDirection, float distanceThroughAtmosphere)
{
    float timeOfDay = fract(foliageMaterial.sunDirectionTimeOfDay.w);
    vec3 cameraToSunLight = normalize(foliageMaterial.sunDirectionTimeOfDay.xyz);
    float viewSunDot = dot(worldDirection, cameraToSunLight);
    float distanceT = encodeLogDistance(distanceThroughAtmosphere);
    float maxLayer = float(textureSize(atmosphereLutTexture, 0).z - 1);
    float layerCoord = distanceT * maxLayer;
    float layer0 = floor(layerCoord);
    float layer1 = min(layer0 + 1.0, maxLayer);
    float layerBlend = layerCoord - layer0;
    vec2 lutUv = vec2(timeOfDay, (viewSunDot * 0.5) + 0.5);
    vec4 sample0 = texture(atmosphereLutTexture, vec3(lutUv, layer0));
    vec4 sample1 = texture(atmosphereLutTexture, vec3(lutUv, layer1));
    return mix(sample0, sample1, layerBlend);
}

float backgroundAtmosphereDistance(vec3 worldDirection)
{
    const vec3 worldUp = vec3(0.0, 1.0, 0.0);
    float cameraAltitude = foliageMaterial.atmosphereParams.w;
    float topPlaneHeight = foliageMaterial.atmosphereParams.x - cameraAltitude;
    float maxDistance = max(foliageMaterial.atmosphereParams.y, 0.00001);
    float upDenominator = dot(worldDirection, worldUp);
    if (topPlaneHeight > 0.0 && upDenominator > 0.00001)
    {
        return min(topPlaneHeight / upDenominator, maxDistance);
    }

    return maxDistance;
}

vec3 sampleSkyRadiance(vec3 worldDirection)
{
    vec3 sampleDirection = transpose(mat3(foliageMaterial.skyRotation)) * worldDirection;
    vec3 skyboxColor = texture(skyboxTexture, sampleDirection).rgb;
    float distanceThroughAtmosphere = backgroundAtmosphereDistance(worldDirection);
    vec4 atmosphere = sampleAtmosphere(worldDirection, distanceThroughAtmosphere);
    return mix(skyboxColor, atmosphere.rgb, atmosphere.a);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - saturate(cosTheta), 5.0);
}

float luminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

float distributionGgx(float nDotH, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float denominator = (nDotH * nDotH) * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(kPi * denominator * denominator, 0.0001);
}

float geometrySchlickGgx(float nDotV, float roughness)
{
    float k = pow(roughness + 1.0, 2.0) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 0.0001);
}

float geometrySmith(float nDotV, float nDotL, float roughness)
{
    return geometrySchlickGgx(nDotV, roughness) * geometrySchlickGgx(nDotL, roughness);
}

void main()
{
    if (fragLodIndex == 2u)
    {
        float nearbyDistanceMeters = length(fragViewPosition.xz);
        float lod2FadeAlpha = saturate(
            (kNearbyLod2FadeStartMeters - nearbyDistanceMeters) /
            max(kNearbyLod2FadeStartMeters - kNearbyLod2FadeEndMeters, 0.00001));
        if (lod2FadeAlpha <= 0.0)
        {
            discard;
        }
        if (lod2FadeAlpha < 1.0 && interleavedGradientNoise(gl_FragCoord.xy) > lod2FadeAlpha)
        {
            discard;
        }
    }

    NearbyMaterialGpu material = materialBuffer.materials[fragMaterialIndex];
    float alphaCutoff = material.params.x;
    float baseColorLayer = float(material.layers0.x);
    float normalLayer = float(material.layers0.y);
    float roughnessLayer = float(material.layers0.z);
    float specularLayer = float(material.layers0.w);
    float aoLayer = float(material.layers1.x);
    float subsurfaceLayer = float(material.layers1.y);

    vec4 albedoSample = texture(baseColorTextureArray, vec3(fragUv0, baseColorLayer));
    if (albedoSample.a < alphaCutoff)
    {
        discard;
    }

    vec3 tangentNormal = texture(normalTextureArray, vec3(fragUv0, normalLayer)).xyz * 2.0 - 1.0;
    if (!gl_FrontFacing)
    {
        tangentNormal.xy *= -1.0;
    }
    mat3 tbn = mat3(
        normalize(fragTangent),
        normalize(fragBitangent),
        normalize(fragNormal));
    vec3 normal = normalize(tbn * tangentNormal);
    if (!gl_FrontFacing)
    {
        normal = -normal;
    }

    vec3 sunDirection = normalize(foliageMaterial.sunDirectionIntensity.xyz);
    vec3 viewDirection = normalize(-fragViewPosition);
    vec3 halfVector = normalize(viewDirection + sunDirection);

    float roughness = clamp(texture(roughnessTextureArray, vec3(fragUv0, roughnessLayer)).r, 0.08, 1.0);
    vec3 specularColor = texture(specularTextureArray, vec3(fragUv0, specularLayer)).rgb;
    float ao = texture(aoTextureArray, vec3(fragUv0, aoLayer)).r;
    float effectiveAo = mix(1.0, ao, 0.35);
    vec3 subsurfaceColor = texture(subsurfaceTextureArray, vec3(fragUv0, subsurfaceLayer)).rgb;

    float nDotL = saturate(dot(normal, sunDirection));
    float nDotV = saturate(dot(normal, viewDirection));
    float nDotH = saturate(dot(normal, halfVector));
    float vDotH = saturate(dot(viewDirection, halfVector));

    vec3 dielectricF0 = vec3(foliageMaterial.shadingParams0.x);
    float specularStrength = saturate(luminance(specularColor));
    vec3 f0 = dielectricF0 * mix(0.22, 0.72, specularStrength);
    f0 = clamp(f0, vec3(0.008), vec3(0.045));
    vec3 fresnel = fresnelSchlick(vDotH, f0);
    float distribution = distributionGgx(nDotH, roughness);
    float geometry = geometrySmith(max(nDotV, 0.05), max(nDotL, 0.05), roughness);
    vec3 specular = (distribution * geometry * fresnel) / max(4.0 * max(nDotV, 0.05) * max(nDotL, 0.05), 0.0001);

    vec3 diffuseColor = albedoSample.rgb * (1.0 - fresnel);
    vec3 diffuse = (diffuseColor / kPi) * nDotL;

    float transmissionStrength = foliageMaterial.shadingParams0.z;
    float backScatter = pow(saturate(dot(-viewDirection, sunDirection)), 2.0) * saturate(dot(-normal, sunDirection));
    vec3 transmission = subsurfaceColor * albedoSample.rgb * backScatter * transmissionStrength;

    float directVisibility = sunVisibility(sunDirection.y);
    float ambientVisibility = mix(0.10, 1.0, daylightVisibility(sunDirection.y));
    vec3 sunRadiance = foliageMaterial.sunColorAmbient.rgb * foliageMaterial.sunDirectionIntensity.w * directVisibility;
    vec3 directLighting =
        (diffuse * sunRadiance * 1.18) +
        (specular * sunRadiance * 0.06) +
        (transmission * sunRadiance * 1.28);

    vec3 reflectionDirection = reflect(-viewDirection, normal);
    vec3 reflectedSky = sampleSkyRadiance(reflectionDirection);
    vec3 environmentSpecular =
        reflectedSky *
        fresnelSchlick(nDotV, f0) *
        mix(0.16, 0.05, roughness) *
        mix(0.35, 0.7, pow(1.0 - nDotV, 0.35));
    environmentSpecular *= mix(0.8, 1.0, effectiveAo) * 0.28;

    float ambientScale = foliageMaterial.sunColorAmbient.a * ambientVisibility;
    vec3 skyFill = sampleSkyRadiance(normal) * albedoSample.rgb * kFoliageSkyFillStrength * ambientVisibility;
    vec3 ambient = albedoSample.rgb * ambientScale * mix(0.92, 1.0, effectiveAo) * kFoliageAmbientBoost;
    vec3 litColor = (ambient + skyFill + directLighting + environmentSpecular) * kFoliageLightingScale;

    outColor = vec4(litColor, albedoSample.a);
}
