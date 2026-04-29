#version 450

layout(set=1, binding=0) uniform WaterUniforms
{
    mat4 viewProjection;
    vec4 cameraAndTime;
    vec4 waterParams;
    vec4 sunDirectionIntensity;
    vec4 sunColorAmbient;
    vec4 debugParams;
} water;

struct WaterInstance
{
    vec3 position;
    uint packedMetadata;
    vec4 leafParams;
};

layout(set=0, binding=0, std430) readonly buffer InstanceBuffer
{
    WaterInstance instanceData[];
} instanceBuffer;

layout(location = 0) in vec2 inLocalCoord;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragWorldPosition;
layout(location = 2) out float fragLodBlend;
layout(location = 3) out vec2 fragCameraWorldXZ;
layout(location = 4) out vec4 fragSunDirectionIntensity;
layout(location = 5) out vec4 fragSunColorAmbient;
layout(location = 6) out vec4 fragWaterDebugParams;

const float kTau = 6.28318530718;

vec2 safeNormalize(vec2 value)
{
    float lengthSquared = dot(value, value);
    if (lengthSquared <= 0.000001)
    {
        return vec2(1.0, 0.0);
    }

    return value * inversesqrt(lengthSquared);
}

void accumulateWave(
    vec2 worldXZ,
    float timeSeconds,
    vec2 direction,
    float wavelength,
    float speed,
    float amplitude,
    float choppiness,
    inout vec3 displacement,
    inout vec2 slope)
{
    vec2 waveDirection = safeNormalize(direction);
    float frequency = kTau / wavelength;
    float phase = dot(waveDirection, worldXZ) * frequency + (timeSeconds * speed);
    float sinPhase = sin(phase);
    float cosPhase = cos(phase);

    displacement.xz += waveDirection * (cosPhase * amplitude * choppiness);
    displacement.y += sinPhase * amplitude;
    slope += waveDirection * (cosPhase * amplitude * frequency);
}

void main()
{
    WaterInstance instance = instanceBuffer.instanceData[gl_InstanceIndex];
    float leafSize = instance.leafParams.x;
    float waterLevel = instance.leafParams.y;
    uint waterMeshLod = (instance.packedMetadata >> 8u) & 0xFFu;
    float timeSeconds = water.cameraAndTime.w;
    float amplitudeScale = max(water.waterParams.y, 0.0);
    float choppiness = max(water.waterParams.z, 0.0);

    vec2 localMeters = inLocalCoord * leafSize;
    vec3 position = vec3(
        instance.position.x + localMeters.x,
        instance.position.y + waterLevel,
        instance.position.z + localMeters.y);
    vec2 worldXZ = water.cameraAndTime.xy + position.xz;

    vec3 displacement = vec3(0.0);
    vec2 slope = vec2(0.0);

    accumulateWave(
        worldXZ,
        timeSeconds,
        vec2(0.96, 0.28),
        180.0,
        0.42,
        amplitudeScale * 5.0,
        choppiness * 0.18,
        displacement,
        slope);
    accumulateWave(
        worldXZ,
        timeSeconds,
        vec2(-0.34, 0.94),
        72.0,
        0.86,
        amplitudeScale * 1.8,
        choppiness * 0.12,
        displacement,
        slope);
    accumulateWave(
        worldXZ,
        timeSeconds,
        vec2(0.58, -0.81),
        28.0,
        1.35,
        amplitudeScale * 0.65,
        choppiness * 0.08,
        displacement,
        slope);

    position += displacement;

    fragNormal = normalize(vec3(-slope.x, 1.0, -slope.y));
    fragWorldPosition = position;
    fragLodBlend = clamp(float(waterMeshLod) / 4.0, 0.0, 1.0);
    fragCameraWorldXZ = water.cameraAndTime.xy;
    fragSunDirectionIntensity = water.sunDirectionIntensity;
    fragSunColorAmbient = water.sunColorAmbient;
    fragWaterDebugParams = vec4(
        amplitudeScale,
        timeSeconds,
        water.debugParams.x,
        0.0);
    gl_Position = water.viewProjection * vec4(position, 1.0);
}
