#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragWorldPosition;
layout(location = 2) in float fragLodBlend;
layout(location = 3) in vec2 fragCameraWorldXZ;
layout(location = 4) flat in vec4 fragSunDirectionIntensity;
layout(location = 5) flat in vec4 fragSunColorAmbient;
layout(location = 6) flat in vec4 fragWaterDebugParams;

layout(location = 0) out vec4 outColor;

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

void accumulateWaveSlope(
    vec2 worldXZ,
    float timeSeconds,
    vec2 direction,
    float wavelength,
    float speed,
    float amplitude,
    inout vec2 slope)
{
    vec2 waveDirection = safeNormalize(direction);
    float frequency = kTau / wavelength;
    float phase = dot(waveDirection, worldXZ) * frequency + (timeSeconds * speed);
    float cosPhase = cos(phase);
    slope += waveDirection * (cosPhase * amplitude * frequency);
}

void main()
{
    float amplitudeScale = max(fragWaterDebugParams.x, 0.0);
    float timeSeconds = fragWaterDebugParams.y;
    vec2 worldXZ = fragCameraWorldXZ + fragWorldPosition.xz;
    vec2 slope = vec2(0.0);
    accumulateWaveSlope(worldXZ, timeSeconds, vec2(0.96, 0.28), 180.0, 0.42, amplitudeScale * 5.0, slope);
    accumulateWaveSlope(worldXZ, timeSeconds, vec2(-0.34, 0.94), 72.0, 0.86, amplitudeScale * 1.8, slope);
    accumulateWaveSlope(worldXZ, timeSeconds, vec2(0.58, -0.81), 28.0, 1.35, amplitudeScale * 0.65, slope);

    vec3 normal = normalize(vec3(-slope.x, 1.0, -slope.y));
    vec3 viewDir = normalize(-fragWorldPosition);
    vec3 sunDirection = normalize(fragSunDirectionIntensity.xyz);
    float diffuse = max(dot(normal, sunDirection), 0.0) * fragSunDirectionIntensity.w;
    float fresnel = pow(1.0 - max(dot(normal, viewDir), 0.0), 5.0);

    vec3 deepColor = vec3(0.03, 0.18, 0.30);
    vec3 shallowColor = vec3(0.08, 0.34, 0.52);
    vec3 baseColor = mix(shallowColor, deepColor, 0.30);
    if (fragWaterDebugParams.z > 0.5)
    {
        vec3 lodTint = mix(vec3(0.10, 0.20, 0.28), vec3(0.03, 0.12, 0.18), fragLodBlend);
        baseColor += lodTint * 0.25;
    }

    vec3 ambient = fragSunColorAmbient.rgb * (fragSunColorAmbient.a * 1.35);
    vec3 specular = vec3(pow(max(dot(reflect(-sunDirection, normal), viewDir), 0.0), 48.0)) * 0.18;
    vec3 color = (baseColor * (ambient + (fragSunColorAmbient.rgb * diffuse))) + specular;
    color = mix(color, vec3(0.86, 0.94, 1.0), fresnel * 0.35);

    outColor = vec4(color, 1.0);
}
