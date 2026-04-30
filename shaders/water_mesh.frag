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
    vec4 depthEffectParams;
} water;

layout(set=2, binding=0) uniform sampler2DArray displacementTexture;
layout(set=2, binding=1) uniform sampler2DArray slopeTexture;

layout(location = 0) in vec3 fragWorldPosition;
layout(location = 1) flat in uint fragBandMask;
layout(location = 2) in float fragShoreFactor;

layout(location = 0) out vec4 outColor;

float cascadeWorldSize(uint cascadeIndex)
{
    if (cascadeIndex < 4u)
    {
        return water.cascadeWorldSizesA[cascadeIndex];
    }

    return water.cascadeWorldSizesB[cascadeIndex - 4u];
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

    vec3 normal = normalize(vec3(-slope.x, 1.0, -slope.y));
    vec3 viewDir = normalize(-fragWorldPosition);
    vec3 sunDirection = normalize(water.sunDirectionIntensity.xyz);
    float diffuse = max(dot(normal, sunDirection), 0.0) * water.sunDirectionIntensity.w;
    float fresnel = pow(1.0 - max(dot(normal, viewDir), 0.0), 5.0);

    vec3 deepColor = vec3(0.03, 0.18, 0.30);
    vec3 shallowColor = vec3(0.08, 0.34, 0.52);
    vec3 baseColor = mix(shallowColor, deepColor, 0.30);
    baseColor = mix(baseColor, vec3(0.16, 0.42, 0.46), clamp(fragShoreFactor, 0.0, 1.0) * 0.65);
    baseColor += vec3(0.02, 0.03, 0.04) * clamp(displacementEnergy * 0.01, 0.0, 1.0);
    if (water.debugParams.x > 0.5)
    {
        vec3 lodTint = vec3(0.10, 0.20, 0.28);
        baseColor += lodTint * 0.25;
    }

    vec3 ambient = water.sunColorAmbient.rgb * (water.sunColorAmbient.a * 1.35);
    vec3 specular = vec3(pow(max(dot(reflect(-sunDirection, normal), viewDir), 0.0), 48.0)) * 0.18;
    vec3 color = (baseColor * (ambient + (water.sunColorAmbient.rgb * diffuse))) + specular;
    color = mix(color, vec3(0.86, 0.94, 1.0), fresnel * 0.35);

    outColor = vec4(color, 1.0);
}
