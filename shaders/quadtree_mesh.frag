#version 450

layout(set=3, binding=0) uniform TerrainUniforms
{
    mat4 viewProjection;
    vec4 sunDirectionIntensity;
    vec4 sunColorAmbient;
    vec4 terrainHeightParams;
    vec4 cameraWorldAndTime;
    vec4 waterCausticsParams;
    vec4 waterCascadeWorldSizesA;
    vec4 waterCascadeWorldSizesB;
    vec4 waterCausticsPatternParams;
    vec4 waterCausticsRidgeParamsA;
    vec4 waterCausticsRidgeParamsB;
    vec4 waterCausticsDecodeParams;
    vec4 waterCausticsRotationParams;
} terrain;

layout(set=2, binding=0) uniform sampler2DArray displacementTexture;
layout(set=2, binding=1) uniform sampler2DArray slopeTexture;
layout(set=2, binding=2) uniform sampler2D causticsTextureA;

layout(location = 0) in vec3 fragLocalPosition;
layout(location = 1) in vec3 fragWorldNormal;
layout(location = 2) flat in uint fragAllowCaustics;

layout(location = 0) out vec4 outColor;

float saturate(float value)
{
    return clamp(value, 0.0, 1.0);
}

vec3 terrainAlbedo(float height)
{
    float baseHeight = terrain.terrainHeightParams.x;
    float heightAmplitude = max(terrain.terrainHeightParams.y, 0.001);
    float normalizedHeight = clamp((height - baseHeight) / (heightAmplitude * 1.8), 0.0, 1.0);
    vec3 lowland = vec3(0.14, 0.34, 0.16);
    vec3 highland = vec3(0.46, 0.40, 0.31);
    return mix(lowland, highland, normalizedHeight);
}

vec3 applyShorelineSand(vec3 albedo, float height)
{
    float waterHeight = terrain.waterCausticsParams.x;
    float sandBand =
        smoothstep(waterHeight - 10.0, waterHeight - 0.25, height) *
        (1.0 - smoothstep(waterHeight + 6.0, waterHeight + 10.0, height));

    vec3 drySand = vec3(0.78, 0.70, 0.52);
    vec3 transitionSand = vec3(0.70, 0.63, 0.47);
    vec3 wetSand = vec3(0.56, 0.50, 0.36);

    float wetT = 1.0 - smoothstep(waterHeight - 3.0, waterHeight + 2.5, height);
    vec3 dryToTransition = mix(transitionSand, drySand, smoothstep(waterHeight + 0.5, waterHeight + 5.0, height));
    vec3 shorelineColor = mix(dryToTransition, wetSand, wetT);
    return mix(albedo, shorelineColor, sandBand);
}

float cascadeWorldSize(uint cascadeIndex)
{
    if (cascadeIndex < 4u)
    {
        return terrain.waterCascadeWorldSizesA[cascadeIndex];
    }

    return terrain.waterCascadeWorldSizesB[cascadeIndex - 4u];
}

void sampleWaterDrivenCausticsState(
    vec2 worldXZ,
    out vec2 displacementWarp,
    out vec2 slopeWarp,
    out float focusSignal)
{
    displacementWarp = vec2(0.0);
    slopeWarp = vec2(0.0);
    focusSignal = 0.0;
    float totalWeight = 0.0;
    uint cascadeCount = uint(max(terrain.waterCausticsParams.z, 0.0));

    for (uint cascadeIndex = 0u; cascadeIndex < cascadeCount; ++cascadeIndex)
    {
        float worldSize = max(cascadeWorldSize(cascadeIndex), 1.0);
        vec2 uv = fract(worldXZ / worldSize);
        vec3 displacement = texture(displacementTexture, vec3(uv, float(cascadeIndex))).xyz;
        vec2 slope = texture(slopeTexture, vec3(uv, float(cascadeIndex))).xy;
        float detailWeight = 1.0 - smoothstep(500.0, 8000.0, worldSize);
        float weight = mix(0.18, 1.0, detailWeight);
        displacementWarp += displacement.xz * weight;
        slopeWarp += slope * weight;
        focusSignal += length(slope) * mix(0.55, 1.15, detailWeight);
        totalWeight += weight;
    }

    if (totalWeight > 0.0)
    {
        displacementWarp /= totalWeight;
        slopeWarp /= totalWeight;
        focusSignal /= totalWeight;
    }
}

float causticsPattern(vec2 uv, vec2 waterWarp, float focusSignal)
{
    mat2 rotateA = mat2(
        terrain.waterCausticsRotationParams.x,
        -terrain.waterCausticsRotationParams.y,
        terrain.waterCausticsRotationParams.y,
        terrain.waterCausticsRotationParams.x);
    mat2 rotateB = mat2(
        terrain.waterCausticsRotationParams.z,
        -terrain.waterCausticsRotationParams.w,
        terrain.waterCausticsRotationParams.w,
        terrain.waterCausticsRotationParams.z);
    vec2 uvA = (rotateA * (uv + (waterWarp * 0.85))) * terrain.waterCausticsPatternParams.x;
    vec2 uvB = (rotateB * (uv - (waterWarp * 1.05))) * terrain.waterCausticsPatternParams.y;

    float sdfA = (texture(causticsTextureA, uvA).r - 0.5) * terrain.waterCausticsDecodeParams.x;
    float sdfB = (texture(causticsTextureA, uvB).r - 0.5) * terrain.waterCausticsDecodeParams.y;
    float ridgeA = 1.0 - smoothstep(terrain.waterCausticsRidgeParamsA.x, terrain.waterCausticsRidgeParamsA.y, sdfA);
    float ridgeB = 1.0 - smoothstep(terrain.waterCausticsRidgeParamsB.x, terrain.waterCausticsRidgeParamsB.y, sdfB);
    float combined = max(ridgeA, ridgeB * 0.92);
    combined = smoothstep(0.18, 0.92, combined);
    float focusBlend = smoothstep(terrain.waterCausticsRidgeParamsA.z, terrain.waterCausticsRidgeParamsA.w, focusSignal);
    return pow(saturate(combined), 1.35) * mix(0.45, 1.0, focusBlend);
}

void main()
{
    vec3 worldPosition = terrain.cameraWorldAndTime.xyz + fragLocalPosition;
    vec3 normal = normalize(fragWorldNormal);
    vec3 sunDirection = normalize(terrain.sunDirectionIntensity.xyz);

    float diffuse = max(dot(normal, sunDirection), 0.0) * terrain.sunDirectionIntensity.w;
    vec3 ambient = terrain.sunColorAmbient.rgb * terrain.sunColorAmbient.a;
    vec3 albedo = terrainAlbedo(worldPosition.y);
    albedo = applyShorelineSand(albedo, worldPosition.y);
    vec3 litColor = albedo * (ambient + (terrain.sunColorAmbient.rgb * diffuse));

    if (terrain.waterCausticsParams.y > 0.5 &&
        fragAllowCaustics != 0u &&
        worldPosition.y < terrain.waterCausticsParams.x &&
        abs(normal.y) > terrain.waterCausticsRidgeParamsB.z)
    {
        float waterDepth = terrain.waterCausticsParams.x - worldPosition.y;
        float shoreFade = smoothstep(0.8, 3.0, waterDepth);
        float depthFade = 1.0 - smoothstep(5.0, 16.0, waterDepth);
        float sunFade = smoothstep(0.08, 0.55, sunDirection.y);
        float slopeFade = mix(0.55, 1.0, abs(normal.y));
        vec2 displacementWarp = vec2(0.0);
        vec2 slopeWarp = vec2(0.0);
        float focusSignal = 0.0;
        sampleWaterDrivenCausticsState(worldPosition.xz, displacementWarp, slopeWarp, focusSignal);
        vec2 waterWarp =
            (displacementWarp * terrain.waterCausticsPatternParams.z) +
            (slopeWarp * terrain.waterCausticsPatternParams.w);
        float caustics = causticsPattern(worldPosition.xz, waterWarp, focusSignal);
        float causticsStrength = caustics * shoreFade * depthFade * sunFade * slopeFade * terrain.waterCausticsParams.w;
        litColor += terrain.sunColorAmbient.rgb * terrain.sunDirectionIntensity.w * causticsStrength;
    }

    outColor = vec4(litColor, 1.0);
}
