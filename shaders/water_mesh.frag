#version 450

layout(set=3, binding=0) uniform WaterUniforms
{
    mat4 viewProjection;
    vec4 cameraAndTime;
    vec4 waterParams;
    vec4 sunDirectionIntensity;
    vec4 sunColorAmbient;
    vec4 debugParams;
    vec4 cascadeWorldSizesA;
    vec4 cascadeWorldSizesB;
    vec4 cascadeShallowDampingA;
    vec4 cascadeShallowDampingB;
    vec4 depthEffectParams;
    mat4 skyRotation;
    vec4 atmosphereParams;
    vec4 sunDirectionTimeOfDay;
    vec4 opticalParams;
    vec4 refractionParams;
} water;

layout(set=2, binding=0) uniform sampler2DArray displacementTexture;
layout(set=2, binding=1) uniform sampler2DArray slopeTexture;
layout(set=2, binding=2) uniform samplerCube skyboxTexture;
layout(set=2, binding=3) uniform sampler2DArray atmosphereLutTexture;
layout(set=2, binding=4, std430) readonly buffer HeightmapBuffer
{
    float heights[];
} heightmapBuffer;

layout(location = 0) in vec3 fragWorldPosition;
layout(location = 1) flat in uint fragBandMask;
layout(location = 2) in float fragShoreFactor;
layout(location = 3) in float fragLocalDepth;
layout(location = 4) in vec2 fragLocalMeters;
layout(location = 5) flat in uint fragTerrainSliceIndex;
layout(location = 6) flat in float fragLeafSize;
layout(location = 7) flat in uint fragHasTerrainSlice;

layout(location = 0) out vec4 outColor;

const float kPi = 3.14159265358979323846;
const uint kHeightmapResolution = 259u;
const uint kHeightmapMaxCoord = kHeightmapResolution - 1u;
const float kHeightmapLeafIntervalCount = 256.0;

float cascadeWorldSize(uint cascadeIndex)
{
    if (cascadeIndex < 4u)
    {
        return water.cascadeWorldSizesA[cascadeIndex];
    }

    return water.cascadeWorldSizesB[cascadeIndex - 4u];
}

float sampleTerrainHeight(uint sliceIndex, vec2 localMeters, float leafSize)
{
    float sampleSpacing = leafSize / kHeightmapLeafIntervalCount;
    vec2 sampleCoord = vec2(1.0) + (localMeters / max(sampleSpacing, 1.0e-5));
    ivec2 clampedCoord = clamp(ivec2(round(sampleCoord)), ivec2(0), ivec2(int(kHeightmapMaxCoord)));
    uint linearIndex =
        (sliceIndex * kHeightmapResolution * kHeightmapResolution) +
        (uint(clampedCoord.y) * kHeightmapResolution) +
        uint(clampedCoord.x);
    return heightmapBuffer.heights[linearIndex];
}

vec3 computeTerrainNormal(uint sliceIndex, vec2 localMeters, float leafSize)
{
    float sampleSpacing = leafSize / kHeightmapLeafIntervalCount;
    vec2 sampleOffsetX = vec2(sampleSpacing, 0.0);
    vec2 sampleOffsetZ = vec2(0.0, sampleSpacing);

    float hL = sampleTerrainHeight(sliceIndex, localMeters - sampleOffsetX, leafSize);
    float hR = sampleTerrainHeight(sliceIndex, localMeters + sampleOffsetX, leafSize);
    float hD = sampleTerrainHeight(sliceIndex, localMeters - sampleOffsetZ, leafSize);
    float hU = sampleTerrainHeight(sliceIndex, localMeters + sampleOffsetZ, leafSize);

    vec3 tangentX = vec3(2.0 * sampleSpacing, hR - hL, 0.0);
    vec3 tangentZ = vec3(0.0, hU - hD, 2.0 * sampleSpacing);
    return normalize(cross(tangentZ, tangentX));
}

vec3 terrainAlbedo(float height)
{
    float baseHeight = 0.0;
    float heightAmplitude = 3000.0;
    float normalizedHeight = clamp((height - baseHeight) / (heightAmplitude * 1.8), 0.0, 1.0);
    vec3 lowland = vec3(0.14, 0.34, 0.16);
    vec3 highland = vec3(0.46, 0.40, 0.31);
    return mix(lowland, highland, normalizedHeight);
}

vec3 shadeTerrainBelowWater(vec2 localMeters)
{
    float terrainHeight = sampleTerrainHeight(fragTerrainSliceIndex, localMeters, fragLeafSize);
    vec3 terrainNormal = computeTerrainNormal(fragTerrainSliceIndex, localMeters, fragLeafSize);
    vec3 sunDirection = normalize(water.sunDirectionIntensity.xyz);
    float diffuse = max(dot(terrainNormal, sunDirection), 0.0) * water.sunDirectionIntensity.w;
    vec3 ambient = water.sunColorAmbient.rgb * water.sunColorAmbient.a;
    return terrainAlbedo(terrainHeight) * (ambient + (water.sunColorAmbient.rgb * diffuse));
}

bool traceShallowRefraction(vec3 waterNormal, vec3 viewDir, out vec2 hitLocalMeters, out vec3 hitTerrainColor)
{
    if (fragHasTerrainSlice == 0u)
    {
        return false;
    }

    if (fragLocalDepth > max(water.refractionParams.y, 0.01))
    {
        return false;
    }

    vec3 refractedDir = refract(-viewDir, waterNormal, 1.0 / 1.333);
    if (refractedDir.y >= -0.001)
    {
        return false;
    }

    float maxTravel = fragLocalDepth / max(-refractedDir.y, 0.05);
    vec2 previousLocalMeters = fragLocalMeters;
    float previousDifference = fragLocalDepth;
    const int stepCount = 6;

    for (int stepIndex = 1; stepIndex <= stepCount; ++stepIndex)
    {
        float t = maxTravel * (float(stepIndex) / float(stepCount));
        vec2 localMeters = fragLocalMeters + (refractedDir.xz * t);
        if (localMeters.x < 0.0 || localMeters.y < 0.0 || localMeters.x > fragLeafSize || localMeters.y > fragLeafSize)
        {
            return false;
        }

        float rayHeight = (fragWorldPosition.y - water.waterParams.x) + water.waterParams.x + (refractedDir.y * t);
        float terrainHeight = sampleTerrainHeight(fragTerrainSliceIndex, localMeters, fragLeafSize);
        float difference = rayHeight - terrainHeight;
        if (difference <= 0.0)
        {
            float blend = clamp(previousDifference / max(previousDifference - difference, 1.0e-4), 0.0, 1.0);
            hitLocalMeters = mix(localMeters, previousLocalMeters, blend);
            hitTerrainColor = shadeTerrainBelowWater(hitLocalMeters);
            return true;
        }

        previousLocalMeters = localMeters;
        previousDifference = difference;
    }

    hitLocalMeters = previousLocalMeters;
    hitTerrainColor = shadeTerrainBelowWater(hitLocalMeters);
    return true;
}

float saturate(float value)
{
    return clamp(value, 0.0, 1.0);
}

vec3 saturate(vec3 value)
{
    return clamp(value, vec3(0.0), vec3(1.0));
}

float encodeLogDistance(float distanceThroughAtmosphere)
{
    const float logBase = 256.0;
    float normalizedDistance = saturate(distanceThroughAtmosphere / max(water.atmosphereParams.y, 0.00001));
    return log(normalizedDistance * (logBase - 1.0) + 1.0) / log(logBase);
}

vec4 sampleAtmosphere(vec3 worldDirection, float distanceThroughAtmosphere)
{
    float timeOfDay = fract(water.sunDirectionTimeOfDay.w);
    vec3 cameraToSunLight = normalize(water.sunDirectionTimeOfDay.xyz);
    float viewSunDot = dot(worldDirection, cameraToSunLight);
    float distanceT = encodeLogDistance(distanceThroughAtmosphere);
    float layerCoord = distanceT * float(textureSize(atmosphereLutTexture, 0).z - 1);
    float layer0 = floor(layerCoord);
    float layer1 = min(layer0 + 1.0, float(textureSize(atmosphereLutTexture, 0).z - 1));
    float layerBlend = layerCoord - layer0;
    vec2 lutUv = vec2(timeOfDay, (viewSunDot * 0.5) + 0.5);
    vec4 sample0 = texture(atmosphereLutTexture, vec3(lutUv, layer0));
    vec4 sample1 = texture(atmosphereLutTexture, vec3(lutUv, layer1));
    return mix(sample0, sample1, layerBlend);
}

float backgroundAtmosphereDistance(vec3 worldDirection)
{
    const vec3 worldUp = vec3(0.0, 1.0, 0.0);
    float cameraAltitude = water.atmosphereParams.w;
    float topPlaneHeight = water.atmosphereParams.x - cameraAltitude;
    float upDenominator = dot(worldDirection, worldUp);
    if (topPlaneHeight > 0.0 && upDenominator > 0.00001)
    {
        return topPlaneHeight / upDenominator;
    }

    if (cameraAltitude > 0.0 && upDenominator < -0.00001)
    {
        return cameraAltitude / -upDenominator;
    }

    return min(max(topPlaneHeight, 0.0), water.atmosphereParams.y);
}

vec3 sampleSkyRadiance(vec3 worldDirection)
{
    vec3 sampleDirection = transpose(mat3(water.skyRotation)) * worldDirection;
    vec3 skyboxColor = texture(skyboxTexture, sampleDirection).rgb;
    float distanceThroughAtmosphere = backgroundAtmosphereDistance(worldDirection);
    vec4 atmosphere = sampleAtmosphere(worldDirection, distanceThroughAtmosphere);
    return mix(skyboxColor, atmosphere.rgb, atmosphere.a);
}

float beckmannDistribution(float normalDotHalf, float roughness)
{
    float alpha = max(roughness * roughness, 0.001);
    float alpha2 = alpha * alpha;
    float cos2 = max(normalDotHalf * normalDotHalf, 0.0001);
    float tan2 = max((1.0 - cos2) / cos2, 0.0);
    float exponent = -tan2 / max(alpha2, 0.0001);
    return exp(exponent) / max(kPi * alpha2 * cos2 * cos2, 0.0001);
}

float smithSchlickVisibility(float normalDotView, float normalDotLight, float roughness)
{
    float alpha = max(roughness * roughness, 0.001);
    float k = alpha * 0.5;
    float viewTerm = normalDotView / max(mix(normalDotView, 1.0, k), 0.0001);
    float lightTerm = normalDotLight / max(mix(normalDotLight, 1.0, k), 0.0001);
    return viewTerm * lightTerm;
}

vec3 fresnelSchlick(float cosTheta, vec3 f0)
{
    float factor = pow(1.0 - saturate(cosTheta), 5.0);
    return f0 + (1.0 - f0) * factor;
}

float phaseSchlick(float viewLightDot, float anisotropy)
{
    float k = 1.55 * anisotropy - (0.55 * anisotropy * anisotropy * anisotropy);
    float denominator = 1.0 + (k * viewLightDot);
    return (1.0 - (k * k)) / max(4.0 * kPi * denominator * denominator, 0.0001);
}

void main()
{
    vec2 worldXZ = water.cameraAndTime.xy + fragWorldPosition.xz;
    uint cascadeCount = uint(max(water.waterParams.w, 0.0));

    vec2 slope = vec2(0.0);
    float displacementEnergy = 0.0;
    for (uint cascadeIndex = 0u; cascadeIndex < cascadeCount; ++cascadeIndex)
    {
        if ((fragBandMask & (1u << cascadeIndex)) == 0u)
        {
            continue;
        }

        float worldSize = max(cascadeWorldSize(cascadeIndex), 1.0);
        vec2 uv = fract(worldXZ / worldSize);
        vec4 displacementSample = texture(displacementTexture, vec3(uv, float(cascadeIndex)));
        vec4 slopeSample = texture(slopeTexture, vec3(uv, float(cascadeIndex)));
        slope += slopeSample.xy;
        displacementEnergy += dot(displacementSample.xyz, displacementSample.xyz);
    }

    float slopeMagnitude = length(slope);
    float roughness = clamp(
        water.opticalParams.y + (slopeMagnitude * water.opticalParams.z),
        0.02,
        0.4);

    vec3 normal = normalize(vec3(-slope.x, 1.0, -slope.y));
    vec3 viewDir = normalize(-fragWorldPosition);
    vec3 sunDirection = normalize(water.sunDirectionIntensity.xyz);
    vec3 halfVector = normalize(sunDirection + viewDir);
    float normalDotLight = saturate(dot(normal, sunDirection));
    float normalDotView = saturate(dot(normal, viewDir));
    float normalDotHalf = saturate(dot(normal, halfVector));
    float viewDotHalf = saturate(dot(viewDir, halfVector));
    vec3 f0 = vec3(water.opticalParams.x);
    vec3 fresnelSpecular = fresnelSchlick(viewDotHalf, f0);
    vec3 fresnelReflection = fresnelSchlick(normalDotView, f0);

    float opticalDepth = clamp(fragLocalDepth, 0.0, 48.0);
    float midDepthFactor = smoothstep(2.0, 12.0, opticalDepth);
    float deepDepthFactor = smoothstep(10.0, 32.0, opticalDepth);
    vec3 shallowColor = vec3(0.24, 0.58, 0.60);
    vec3 midColor = vec3(0.08, 0.34, 0.44);
    vec3 deepColor = vec3(0.012, 0.060, 0.115);
    vec3 shoreColor = vec3(0.28, 0.58, 0.52);
    vec3 waterBodyColor = mix(shallowColor, midColor, midDepthFactor);
    waterBodyColor = mix(waterBodyColor, deepColor, deepDepthFactor);
    waterBodyColor = mix(waterBodyColor, shoreColor, clamp(fragShoreFactor, 0.0, 1.0) * 0.65);
    waterBodyColor += vec3(0.015, 0.02, 0.025) * clamp(displacementEnergy * 0.015, 0.0, 1.0);
    if (water.debugParams.x > 0.5)
    {
        vec3 lodTint = vec3(0.10, 0.20, 0.28);
        waterBodyColor += lodTint * 0.25;
    }

    float distribution = beckmannDistribution(normalDotHalf, roughness);
    float visibility = smithSchlickVisibility(normalDotView, normalDotLight, roughness);
    vec3 numerator = distribution * visibility * fresnelSpecular;
    float denominator = max(4.0 * normalDotLight * normalDotView, 0.0001);
    vec3 directSpecular =
        (numerator / denominator) *
        water.sunColorAmbient.rgb *
        (water.sunDirectionIntensity.w * normalDotLight);

    vec3 reflectionDir = reflect(-viewDir, normal);
    vec3 reflectedSky = sampleSkyRadiance(reflectionDir) * water.opticalParams.w;
    vec3 environmentSpecular =
        reflectedSky *
        fresnelReflection *
        mix(1.35, 0.55, roughness) *
        mix(0.45, 1.0, pow(1.0 - normalDotView, 0.35));

    vec3 absorptionCoefficients = vec3(0.22, 0.09, 0.045) * water.debugParams.w;
    vec3 transmission = exp(-absorptionCoefficients * opticalDepth);
    vec3 ambientSky = sampleSkyRadiance(normalize(mix(normal, vec3(0.0, 1.0, 0.0), 0.6)));
    float forwardScatter = phaseSchlick(dot(-viewDir, sunDirection), water.debugParams.z);
    float sunOverhead = smoothstep(0.35, 0.92, sunDirection.y);
    float lookDownFactor = pow(normalDotView, 2.2);
    float deepLookDownFactor = smoothstep(6.0, 28.0, opticalDepth);
    vec3 overheadBlueBoostColor = vec3(0.16, 0.42, 0.58);
    vec3 overheadBlueBoost =
        overheadBlueBoostColor *
        sunOverhead *
        lookDownFactor *
        deepLookDownFactor *
        transmission *
        (1.0 - fresnelReflection) *
        0.45;
    vec3 subsurface =
        ambientSky *
        waterBodyColor *
        transmission *
        (1.0 - fresnelReflection) *
        (water.debugParams.y * mix(1.15, 0.35, deepDepthFactor) * (0.35 + normalDotLight)) *
        (0.45 + forwardScatter * 3.0);

    vec3 ambient =
        ambientSky *
        waterBodyColor *
        transmission *
        (water.sunColorAmbient.a * mix(1.1, 0.55, deepDepthFactor));
    vec3 refractedTerrainColor = vec3(0.0);
    vec2 refractedLocalMeters = fragLocalMeters;
    bool hasRefractedTerrain = traceShallowRefraction(normal, viewDir, refractedLocalMeters, refractedTerrainColor);
    float shallowRefractionStart = water.refractionParams.x;
    float shallowRefractionEnd = max(water.refractionParams.y, shallowRefractionStart + 0.01);
    float shallowRefractionBlend = 0.0;
    if (opticalDepth <= shallowRefractionStart)
    {
        shallowRefractionBlend = 1.0;
    }
    else if (opticalDepth < shallowRefractionEnd)
    {
        float depthT = (opticalDepth - shallowRefractionStart) / (shallowRefractionEnd - shallowRefractionStart);
        float exponentialT = (exp(2.0 * depthT) - 1.0) / (exp(2.0) - 1.0);
        shallowRefractionBlend = 1.0 - exponentialT;
    }
    vec3 shallowTransmission =
        refractedTerrainColor *
        transmission *
        mix(vec3(1.0), waterBodyColor, 0.12);

    vec3 baseWaterLighting = ambient + subsurface;
    if (hasRefractedTerrain)
    {
        baseWaterLighting = mix(baseWaterLighting, shallowTransmission + (ambient * 0.35), shallowRefractionBlend);
    }

    vec3 color = baseWaterLighting + overheadBlueBoost + environmentSpecular + directSpecular;
    color = mix(color, reflectedSky, saturate(max(fresnelReflection.r, max(fresnelReflection.g, fresnelReflection.b)) * 0.65));

    outColor = vec4(color, 1.0);
}
