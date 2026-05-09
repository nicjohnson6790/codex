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
layout(set=2, binding=3) uniform sampler2DArray terrainAlbedoTextureArray;
layout(set=2, binding=4) uniform sampler2DArray terrainNormalTextureArray;
layout(set=2, binding=5) uniform sampler2DArray terrainRoughnessTextureArray;
layout(set=2, binding=6) uniform sampler2DArray terrainAoTextureArray;

layout(location = 0) in vec3 fragLocalPosition;
layout(location = 1) in vec3 fragWorldNormal;
layout(location = 2) flat in uint fragAllowCaustics;

layout(location = 0) out vec4 outColor;

const float kPi = 3.14159265359;
const float kMudLayer = 0.0;
const float kSandLayer = 1.0;
const float kPineLayer = 2.0;
const float kRockLayer = 3.0;
const float kSnowLayer = 4.0;
const float kTerrainPineTop = 850.0;
const float kTerrainRockTop = 2000.0;
const float kTerrainBandBlend = 18.0;
const float kTerrainPineRockBlend = 50.0;

float saturate(float value)
{
    return clamp(value, 0.0, 1.0);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - saturate(cosTheta), 5.0);
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

vec2 terrainUv(vec2 worldXZ, float layer)
{
    float texelWorldSize = 14.0;
    if (layer == kPineLayer)
    {
        texelWorldSize = 8.0;
    }
    else if (layer == kRockLayer)
    {
        texelWorldSize = 18.0;
    }
    else if (layer == kSnowLayer)
    {
        texelWorldSize = 20.0;
    }
    return worldXZ / texelWorldSize;
}

vec4 terrainLayerWeights(float height, vec3 normal, out float snowWeightOut)
{
    float waterHeight = terrain.waterCausticsParams.x;
    float mudToSand = smoothstep(
        waterHeight - 12.5 - kTerrainBandBlend,
        waterHeight - 12.5 + kTerrainBandBlend,
        height);
    float sandToPine = smoothstep(
        waterHeight + 5.0 - kTerrainBandBlend,
        waterHeight + 5.0 + kTerrainBandBlend,
        height);
    float pineToRock = smoothstep(
        kTerrainPineTop - kTerrainPineRockBlend,
        kTerrainPineTop + kTerrainPineRockBlend,
        height);
    float rockToSnow = smoothstep(
        kTerrainRockTop - kTerrainBandBlend,
        kTerrainRockTop + kTerrainBandBlend,
        height);

    float mud = 1.0 - mudToSand;
    float sand = mudToSand * (1.0 - sandToPine);
    float pine = sandToPine * (1.0 - pineToRock);
    float rock = pineToRock * (1.0 - rockToSnow);
    float snow = rockToSnow;

    snowWeightOut = snow;
    return vec4(mud, sand, pine, rock);
}

vec3 sampleLayerAlbedo(vec2 worldXZ, float layer)
{
    return texture(terrainAlbedoTextureArray, vec3(terrainUv(worldXZ, layer), layer)).rgb;
}

vec3 sampleLayerNormal(vec2 worldXZ, float layer)
{
    vec2 encodedNormal = texture(terrainNormalTextureArray, vec3(terrainUv(worldXZ, layer), layer)).rg * 2.0 - 1.0;
    if (layer == kSnowLayer)
    {
        encodedNormal.y *= -1.0;
    }
    float z = sqrt(max(1.0 - dot(encodedNormal, encodedNormal), 0.0));
    return normalize(vec3(encodedNormal, z));
}

float sampleLayerRoughness(vec2 worldXZ, float layer)
{
    return texture(terrainRoughnessTextureArray, vec3(terrainUv(worldXZ, layer), layer)).r;
}

float sampleLayerAo(vec2 worldXZ, float layer)
{
    return texture(terrainAoTextureArray, vec3(terrainUv(worldXZ, layer), layer)).r;
}

void sampleTerrainMaterial(vec2 worldXZ, float height, vec3 geometricNormal, out vec3 albedo, out vec3 normal, out float roughness, out float ao)
{
    float snow = 0.0;
    vec4 weights = terrainLayerWeights(height, geometricNormal, snow);
    float rock = weights.w;

    vec3 weightedAlbedo =
        sampleLayerAlbedo(worldXZ, kMudLayer) * weights.x +
        sampleLayerAlbedo(worldXZ, kSandLayer) * weights.y +
        sampleLayerAlbedo(worldXZ, kPineLayer) * weights.z +
        sampleLayerAlbedo(worldXZ, kRockLayer) * rock +
        sampleLayerAlbedo(worldXZ, kSnowLayer) * snow;

    vec3 weightedTangentNormal =
        sampleLayerNormal(worldXZ, kMudLayer) * weights.x +
        sampleLayerNormal(worldXZ, kSandLayer) * weights.y +
        sampleLayerNormal(worldXZ, kPineLayer) * weights.z +
        sampleLayerNormal(worldXZ, kRockLayer) * rock +
        sampleLayerNormal(worldXZ, kSnowLayer) * snow;

    float weightedRoughness =
        sampleLayerRoughness(worldXZ, kMudLayer) * weights.x +
        sampleLayerRoughness(worldXZ, kSandLayer) * weights.y +
        sampleLayerRoughness(worldXZ, kPineLayer) * weights.z +
        sampleLayerRoughness(worldXZ, kRockLayer) * rock +
        sampleLayerRoughness(worldXZ, kSnowLayer) * snow;

    float weightedAo =
        sampleLayerAo(worldXZ, kMudLayer) * weights.x +
        sampleLayerAo(worldXZ, kSandLayer) * weights.y +
        sampleLayerAo(worldXZ, kPineLayer) * weights.z +
        sampleLayerAo(worldXZ, kRockLayer) * rock +
        sampleLayerAo(worldXZ, kSnowLayer) * snow;

    vec3 tangent = normalize(vec3(1.0, 0.0, 0.0) - geometricNormal * geometricNormal.x);
    vec3 bitangent = normalize(cross(geometricNormal, tangent));
    mat3 tbn = mat3(tangent, bitangent, geometricNormal);

    albedo = weightedAlbedo;
    normal = normalize(tbn * normalize(weightedTangentNormal));
    roughness = clamp(weightedRoughness, 0.08, 1.0);
    ao = mix(1.0, weightedAo, 0.55);
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
    vec3 geometricNormal = normalize(fragWorldNormal);
    vec3 albedo = vec3(0.0);
    vec3 normal = geometricNormal;
    float roughness = 0.8;
    float ao = 1.0;
    sampleTerrainMaterial(worldPosition.xz, worldPosition.y, geometricNormal, albedo, normal, roughness, ao);

    vec3 sunDirection = normalize(terrain.sunDirectionIntensity.xyz);
    vec3 viewDirection = normalize(-fragLocalPosition);
    vec3 halfVector = normalize(viewDirection + sunDirection);

    float nDotL = saturate(dot(normal, sunDirection));
    float nDotV = saturate(dot(normal, viewDirection));
    float nDotH = saturate(dot(normal, halfVector));
    float vDotH = saturate(dot(viewDirection, halfVector));

    vec3 f0 = vec3(0.04);
    vec3 fresnel = fresnelSchlick(vDotH, f0);
    float distribution = distributionGgx(nDotH, roughness);
    float geometry = geometrySmith(max(nDotV, 0.05), max(nDotL, 0.05), roughness);
    vec3 specular = (distribution * geometry * fresnel) / max(4.0 * max(nDotV, 0.05) * max(nDotL, 0.05), 0.0001);
    vec3 diffuse = (albedo * (1.0 - fresnel) / kPi) * nDotL;

    vec3 sunRadiance = terrain.sunColorAmbient.rgb * terrain.sunDirectionIntensity.w;
    vec3 directLighting = (diffuse + (specular * 0.12)) * sunRadiance;
    vec3 ambient = albedo * terrain.sunColorAmbient.a * terrain.sunColorAmbient.rgb * mix(0.45, 1.0, ao);
    vec3 litColor = ambient + directLighting;

    if (terrain.waterCausticsParams.y > 0.5 &&
        fragAllowCaustics != 0u &&
        worldPosition.y < terrain.waterCausticsParams.x &&
        abs(geometricNormal.y) > terrain.waterCausticsRidgeParamsB.z)
    {
        float waterDepth = terrain.waterCausticsParams.x - worldPosition.y;
        float shoreFade = smoothstep(0.8, 3.0, waterDepth);
        float depthFade = 1.0 - smoothstep(5.0, 16.0, waterDepth);
        float sunFade = smoothstep(0.08, 0.55, sunDirection.y);
        float slopeFade = mix(0.55, 1.0, abs(geometricNormal.y));
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
