#version 450

layout(location = 0) in vec2 fragNdc;
layout(location = 0) out vec4 outColor;

layout(set=2, binding=0) uniform samplerCube skyboxTexture;
layout(set=2, binding=1) uniform sampler2DArray atmosphereLutTexture;
layout(set=2, binding=2) uniform sampler2D depthTexture;

layout(set=3, binding=0) uniform SkyboxUniforms
{
    mat4 inverseViewProjection;
    mat4 skyRotation;
    vec4 atmosphereParams;
    vec4 sunDirectionTimeOfDay;
} uniforms;

float saturate(float value)
{
    return clamp(value, 0.0, 1.0);
}

vec3 reconstructWorldPosition(float depth)
{
    vec4 clipPosition = vec4(fragNdc, depth, 1.0);
    vec4 worldPosition = uniforms.inverseViewProjection * clipPosition;
    return worldPosition.xyz / max(worldPosition.w, 0.00001);
}

float encodeLogDistance(float distanceThroughAtmosphere)
{
    const float logBase = 256.0;
    float normalizedDistance = saturate(distanceThroughAtmosphere / max(uniforms.atmosphereParams.y, 0.00001));
    return log(normalizedDistance * (logBase - 1.0) + 1.0) / log(logBase);
}

vec4 sampleAtmosphere(vec3 worldDirection, float distanceThroughAtmosphere)
{
    float timeOfDay = fract(uniforms.sunDirectionTimeOfDay.w);
    vec3 cameraToSunLight = normalize(uniforms.sunDirectionTimeOfDay.xyz);
    float viewSunDot = dot(worldDirection, cameraToSunLight);
    float distanceT = encodeLogDistance(distanceThroughAtmosphere);
    float layerCoord = distanceT * float(textureSize(atmosphereLutTexture, 0).z - 1);
    float layer0 = floor(layerCoord);
    float layer1 = min(layer0 + 1.0, float(textureSize(atmosphereLutTexture, 0).z - 1));
    float layerBlend = layerCoord - layer0;
    vec2 lutUv = vec2(
        timeOfDay,
        (viewSunDot * 0.5) + 0.5
    );
    vec4 sample0 = texture(atmosphereLutTexture, vec3(lutUv, layer0));
    vec4 sample1 = texture(atmosphereLutTexture, vec3(lutUv, layer1));
    return mix(sample0, sample1, layerBlend);
}

float backgroundAtmosphereDistance(vec3 worldDirection)
{
    const vec3 worldUp = vec3(0.0, 1.0, 0.0);
    float cameraAltitude = uniforms.atmosphereParams.w;
    float topPlaneHeight = uniforms.atmosphereParams.x - cameraAltitude;
    float maxDistance = max(uniforms.atmosphereParams.y, 0.00001);
    float upDenominator = dot(worldDirection, worldUp);
    if (topPlaneHeight > 0.0 && upDenominator > 0.00001)
    {
        return min(topPlaneHeight / upDenominator, maxDistance);
    }

    // Treat the atmosphere as a deep medium below the camera instead of
    // intersecting a hard ground plane at y = 0.
    return maxDistance;
}

void main()
{
    vec3 worldDirection = normalize(reconstructWorldPosition(0.0));
    vec3 sampleDirection = transpose(mat3(uniforms.skyRotation)) * worldDirection;
    vec4 skyboxColor = texture(skyboxTexture, sampleDirection);

    ivec2 depthSize = textureSize(depthTexture, 0);
    ivec2 pixelCoord = clamp(ivec2(gl_FragCoord.xy), ivec2(0), depthSize - ivec2(1));
    float depth = texelFetch(depthTexture, pixelCoord, 0).r;

    if (depth > 0.0)
    {
        float distanceThroughAtmosphere = length(reconstructWorldPosition(depth));
        outColor = sampleAtmosphere(worldDirection, distanceThroughAtmosphere);
        return;
    }

    float distanceThroughAtmosphere = backgroundAtmosphereDistance(worldDirection);
    vec4 atmosphere = sampleAtmosphere(worldDirection, distanceThroughAtmosphere);
    outColor = vec4(mix(skyboxColor.rgb, atmosphere.rgb, atmosphere.a), 1.0);
}
