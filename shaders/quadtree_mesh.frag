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
} terrain;

layout(set=2, binding=0) uniform sampler2DArray displacementTexture;
layout(set=2, binding=1) uniform sampler2DArray slopeTexture;

layout(location = 0) in vec3 fragLocalPosition;
layout(location = 1) in vec3 fragWorldNormal;

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

vec2 hash22(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

vec2 worleyF1F2(vec2 uv)
{
    vec2 baseCell = floor(uv);
    vec2 cellUv = fract(uv);
    float f1 = 1.0e9;
    float f2 = 1.0e9;

    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 cellOffset = vec2(float(x), float(y));
            vec2 featurePoint = hash22(baseCell + cellOffset);
            vec2 delta = (cellOffset + featurePoint) - cellUv;
            float distanceSquared = dot(delta, delta);
            if (distanceSquared < f1)
            {
                f2 = f1;
                f1 = distanceSquared;
            }
            else if (distanceSquared < f2)
            {
                f2 = distanceSquared;
            }
        }
    }

    return sqrt(vec2(f1, f2));
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
    const mat2 rotateA = mat2(0.80, -0.60, 0.60, 0.80);
    const mat2 rotateB = mat2(0.58, 0.81, -0.81, 0.58);
    vec2 warpCoord = (uv * 0.22) + (waterWarp * 0.09);
    vec2 warp = vec2(
        sin(warpCoord.x + warpCoord.y * 1.7),
        cos(warpCoord.y - warpCoord.x * 1.3)) * 0.18;

    vec2 uvA = (rotateA * (uv + warp + (waterWarp * 0.85))) * 0.22;
    vec2 uvB = (rotateB * (uv - warp.yx - (waterWarp * 1.05))) * 0.31;

    vec2 fA = worleyF1F2(uvA);
    vec2 fB = worleyF1F2(uvB);

    float ridgeA = 1.0 - smoothstep(0.012, 0.16, fA.y - fA.x);
    float ridgeB = 1.0 - smoothstep(0.010, 0.145, fB.y - fB.x);
    float combined = max(ridgeA, ridgeB * 0.92);
    combined = smoothstep(0.18, 0.92, combined);
    float focusBlend = smoothstep(0.08, 0.42, focusSignal);
    return pow(saturate(combined), 1.35) * mix(0.45, 1.0, focusBlend);
}

float triplanarCaustics(vec3 worldPosition, vec3 worldNormal, vec2 waterWarp, float focusSignal)
{
    vec3 blend = pow(abs(worldNormal), vec3(5.0));
    blend /= max(dot(blend, vec3(1.0)), 1.0e-5);

    vec2 yzWarp = vec2(waterWarp.y, waterWarp.x * 0.5);
    vec2 xzWarp = waterWarp;
    vec2 xyWarp = vec2(waterWarp.x, waterWarp.y * 0.5);
    float yz = causticsPattern(worldPosition.yz, yzWarp, focusSignal);
    float xz = causticsPattern(worldPosition.xz, xzWarp, focusSignal);
    float xy = causticsPattern(worldPosition.xy, xyWarp, focusSignal);
    return (yz * blend.x) + (xz * blend.y) + (xy * blend.z);
}

void main()
{
    vec3 worldPosition = terrain.cameraWorldAndTime.xyz + fragLocalPosition;
    vec3 normal = normalize(fragWorldNormal);
    vec3 sunDirection = normalize(terrain.sunDirectionIntensity.xyz);

    float diffuse = max(dot(normal, sunDirection), 0.0) * terrain.sunDirectionIntensity.w;
    vec3 ambient = terrain.sunColorAmbient.rgb * terrain.sunColorAmbient.a;
    vec3 litColor = terrainAlbedo(worldPosition.y) * (ambient + (terrain.sunColorAmbient.rgb * diffuse));

    if (terrain.waterCausticsParams.y > 0.5 && worldPosition.y < terrain.waterCausticsParams.x)
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
        vec2 waterWarp = (displacementWarp * 0.12) + (slopeWarp * 10.0);
        float caustics = triplanarCaustics(worldPosition, normal, waterWarp, focusSignal);
        float causticsStrength = caustics * shoreFade * depthFade * sunFade * slopeFade * 0.10;
        litColor += terrain.sunColorAmbient.rgb * terrain.sunDirectionIntensity.w * causticsStrength;
    }

    outColor = vec4(litColor, 1.0);
}
