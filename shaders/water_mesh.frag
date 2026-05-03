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
    vec4 distanceLodParams;
    vec4 cascadeFilterParams;
    vec4 farFieldParams;
    vec4 foamLodParams;
    vec4 foamParams;
    vec4 foamParams2;
    vec4 foamColor;
    vec4 foamDetailShape;
    vec4 foamDetailRidges;
    vec4 foamDetailBreakup;
    vec4 foamEvolutionParams;
    vec4 foamFadeParams;
} water;

layout(set=2, binding=0) uniform sampler2DArray displacementTexture;
layout(set=2, binding=1) uniform sampler2DArray slopeTexture;
layout(set=2, binding=2) uniform sampler2DArray foamTexture;
layout(set=2, binding=3) uniform samplerCube skyboxTexture;
layout(set=2, binding=4) uniform sampler2DArray atmosphereLutTexture;
layout(set=2, binding=5) uniform sampler2D foamDetailSdfTexture;
layout(set=2, binding=6) uniform sampler2D foamDetailNoiseTexture;

layout(location = 0) in vec3 fragWorldPosition;
layout(location = 1) flat in uint fragBandMask;
layout(location = 2) in float fragShoreFactor;
layout(location = 3) in float fragLocalDepth;
layout(location = 4) flat in uint fragHasTerrainSlice;
layout(location = 5) in float fragViewDistance;

layout(location = 0) out vec4 outColor;

const float kPi = 3.14159265358979323846;
const float kInvLog256 = 0.18033688011112042;
const float kAtmosphereLutMaxLayer = 31.0;

float cascadeWorldSize(uint cascadeIndex)
{
    if (cascadeIndex < 4u)
    {
        return water.cascadeWorldSizesA[cascadeIndex];
    }

    return water.cascadeWorldSizesB[cascadeIndex - 4u];
}

float saturate(float value)
{
    return clamp(value, 0.0, 1.0);
}

float metersPerPixel(float viewDistance)
{
    float viewportHeight = max(water.distanceLodParams.x, 1.0);
    float tanHalfVerticalFov = max(water.distanceLodParams.y, 1.0e-4);
    return max((2.0 * tanHalfVerticalFov * viewDistance) / viewportHeight, 1.0e-4);
}

float cascadeDetailWeight(float worldSize, float metersPerPixelAtView)
{
    float texelWorldSize = worldSize / 512.0;
    float resolvedTexelScale = texelWorldSize / metersPerPixelAtView;
    return smoothstep(
        water.cascadeFilterParams.x,
        water.cascadeFilterParams.y,
        resolvedTexelScale);
}

float sampleFoamSdf(vec2 uv, vec2 ridgeRange)
{
    float sdf = (texture(foamDetailSdfTexture, uv).r - 0.5) * water.foamDetailShape.y;
    float ridge = 1.0 - smoothstep(ridgeRange.x, ridgeRange.y, abs(sdf));
    float fill = 1.0 - smoothstep(-0.18, 0.08, sdf);
    return saturate(max(ridge, fill * 0.26));
}

float encodeLogDistance(float distanceThroughAtmosphere)
{
    float normalizedDistance = saturate(distanceThroughAtmosphere / max(water.atmosphereParams.y, 0.00001));
    return log(normalizedDistance * 255.0 + 1.0) * kInvLog256;
}

vec4 sampleAtmosphere(vec3 worldDirection, float distanceThroughAtmosphere)
{
    float timeOfDay = fract(water.sunDirectionTimeOfDay.w);
    vec3 cameraToSunLight = normalize(water.sunDirectionTimeOfDay.xyz);
    float viewSunDot = dot(worldDirection, cameraToSunLight);
    float distanceT = encodeLogDistance(distanceThroughAtmosphere);
    float layerCoord = distanceT * kAtmosphereLutMaxLayer;
    float layer0 = floor(layerCoord);
    float layer1 = min(layer0 + 1.0, kAtmosphereLutMaxLayer);
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
    float maxDistance = max(water.atmosphereParams.y, 0.00001);
    float upDenominator = dot(worldDirection, worldUp);
    if (topPlaneHeight > 0.0 && upDenominator > 0.00001)
    {
        return min(topPlaneHeight / upDenominator, maxDistance);
    }

    // Match the skybox path by treating the atmosphere as a deep medium
    // instead of intersecting a hard ground plane at y = 0.
    return maxDistance;
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
    float metersPerPixelAtView = metersPerPixel(fragViewDistance);
    vec2 slope = vec2(0.0);
    float shorelineBias = saturate(fragShoreFactor);
    bool drawFoam = water.foamParams2.z > 0.5;
    float foamCoverage = 0.0;
    float farFoamFade = 1.0 - smoothstep(
        water.foamLodParams.x,
        water.foamLodParams.y,
        fragViewDistance);
    bool evaluateFoam = drawFoam && farFoamFade > 0.0;
    vec4 historyNoiseSample = vec4(0.5);
    vec2 historyOffsetWorld = vec2(0.0);
    bool historyOffsetReady = false;
    for (uint cascadeIndex = 0u; cascadeIndex < cascadeCount; ++cascadeIndex)
    {
        if ((fragBandMask & (1u << cascadeIndex)) == 0u)
        {
            continue;
        }

        float worldSize = max(cascadeWorldSize(cascadeIndex), 1.0);
        float detailWeight = cascadeDetailWeight(worldSize, metersPerPixelAtView);
        if (detailWeight <= 0.0)
        {
            continue;
        }

        vec2 uv = fract(worldXZ / worldSize);
        vec4 slopeSample = texture(slopeTexture, vec3(uv, float(cascadeIndex)));
        slope += slopeSample.xy * detailWeight;
        if (evaluateFoam)
        {
            float foamCascadeWeight = smoothstep(
                water.foamLodParams.z,
                1.0,
                detailWeight);
            if (foamCascadeWeight <= 0.0)
            {
                continue;
            }
            float coverageWeight = 1.0 - (0.55 * (1.0 - smoothstep(500.0, 8000.0, worldSize)));
            float maxCascadeContribution = coverageWeight * foamCascadeWeight;
            if (foamCoverage >= maxCascadeContribution)
            {
                continue;
            }
            if (!historyOffsetReady)
            {
                historyNoiseSample = texture(foamDetailNoiseTexture, worldXZ * water.foamDetailShape.z);
                historyOffsetWorld = (historyNoiseSample.rg - vec2(0.5)) * water.foamDetailShape.w;
                historyOffsetReady = true;
            }
            vec2 historyUv = (worldXZ + historyOffsetWorld) / worldSize;
            float cascadeFoam = saturate(texture(foamTexture, vec3(historyUv, float(cascadeIndex))).r);
            foamCoverage = max(foamCoverage, cascadeFoam * coverageWeight * foamCascadeWeight);
            if (foamCoverage >= 1.0)
            {
                break;
            }
        }
    }
    float foamSignal = 0.0;
    if (evaluateFoam && foamCoverage > water.foamFadeParams.x)
    {
        vec4 worldNoiseSample = historyOffsetReady
            ? historyNoiseSample
            : texture(foamDetailNoiseTexture, worldXZ * water.foamDetailShape.z);
        vec4 breakupNoiseSample = texture(foamDetailNoiseTexture, worldXZ * water.foamDetailBreakup.y);
        vec2 detailOffsetWorld = (worldNoiseSample.ba - vec2(0.5)) * water.foamDetailBreakup.x;
        float historySignal = foamCoverage;
        float decaySignal = 1.0 - historySignal;
        float evolutionRange = max(water.foamEvolutionParams.y - water.foamEvolutionParams.x, 1.0e-4);
        float evolutionT = 1.0 - saturate((decaySignal - water.foamEvolutionParams.x) / evolutionRange);
        if (decaySignal > water.foamEvolutionParams.y)
        {
            float dropoffRange = max(water.foamEvolutionParams.z - water.foamEvolutionParams.y, 1.0e-4);
            float evolutionDropoff = saturate((decaySignal - water.foamEvolutionParams.y) / dropoffRange);
            evolutionT *= evolutionDropoff;
        }
        vec2 evolvedRidgeRange = vec2(
            mix(water.foamDetailRidges.x, water.foamDetailRidges.z, evolutionT),
            mix(water.foamDetailRidges.y, water.foamDetailRidges.w, evolutionT));
        vec2 detailBaseUv = (worldXZ + detailOffsetWorld) / max(water.foamDetailShape.x, 0.0001);
        vec2 detailUv =
            detailBaseUv +
            (slope * 0.022);
        float detailPattern = sampleFoamSdf(detailUv, evolvedRidgeRange);
        float breakupNoise = saturate((breakupNoiseSample.r * 0.55) + (breakupNoiseSample.g * 0.45));
        float breakupMask = mix(1.0 - water.foamDetailBreakup.z, 1.0, smoothstep(0.18, 0.82, breakupNoise));
        detailPattern *= breakupMask;
        detailPattern *= mix(0.94, 1.04, worldNoiseSample.r);
        detailPattern = mix(detailPattern, detailPattern * 1.05, shorelineBias * 0.16);
        float visibilityFade = smoothstep(
            water.foamFadeParams.x,
            water.foamFadeParams.y,
            historySignal);
        float historyPresence = smoothstep(0.001, water.foamFadeParams.y * 1.5, historySignal);
        foamSignal = saturate(detailPattern * visibilityFade * mix(0.72, 1.0, historyPresence)) * farFoamFade;
    }

    float farNormalT = smoothstep(
        water.distanceLodParams.z,
        water.distanceLodParams.w,
        fragViewDistance);
    float farReflectionFlattenT = smoothstep(
        water.farFieldParams.y,
        water.farFieldParams.z,
        fragViewDistance);
    float farRoughnessT = smoothstep(
        water.cascadeFilterParams.z,
        water.cascadeFilterParams.w,
        fragViewDistance);
    float slopeMagnitude = length(slope);
    float roughness = clamp(
        water.opticalParams.y +
        (slopeMagnitude * water.opticalParams.z) +
        (farRoughnessT * water.farFieldParams.x),
        0.02,
        0.65);

    vec3 detailNormal = normalize(vec3(-slope.x, 1.0, -slope.y));
    vec3 normal = normalize(mix(detailNormal, vec3(0.0, 1.0, 0.0), farNormalT));
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
    waterBodyColor = mix(waterBodyColor, shoreColor, shorelineBias * 0.65);
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

    vec3 reflectionNormal = normalize(mix(normal, vec3(0.0, 1.0, 0.0), farReflectionFlattenT));
    vec3 reflectionDir = reflect(-viewDir, reflectionNormal);
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
    vec3 bodyColor = ambient + subsurface + overheadBlueBoost + directSpecular;
    vec3 reflectionColor = mix(
        environmentSpecular,
        reflectedSky,
        saturate(max(fresnelReflection.r, max(fresnelReflection.g, fresnelReflection.b)) * 0.65));
    vec3 foamOverlay = vec3(0.0);

    if (drawFoam && foamSignal > 0.0)
    {
        float foamViewBoost = mix(0.85, 1.15, pow(1.0 - normalDotView, 0.35));
        float foamDiffuse = 0.30 + (0.70 * normalDotLight);
        vec3 foamLighting =
            (ambientSky * 0.55) +
            (water.sunColorAmbient.rgb * water.sunDirectionIntensity.w * foamDiffuse * 0.45);
        vec3 foamColor = water.foamColor.rgb * foamLighting * water.foamColor.a * foamViewBoost;
        foamOverlay = foamColor * (saturate(foamSignal * 0.72) + (foamSignal * 0.12));
    }

    float surfaceAlpha = 1.0;
    if (fragHasTerrainSlice != 0u)
    {
        surfaceAlpha = 1.0 - shallowRefractionBlend;
    }

    vec3 premultipliedColor = (bodyColor * surfaceAlpha) + reflectionColor + foamOverlay;
    outColor = vec4(premultipliedColor, surfaceAlpha);
}
