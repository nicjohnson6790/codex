#version 450

layout(set=1, binding=0) uniform TerrainUniforms
{
    mat4 viewProjection;
    vec4 sunDirectionIntensity;
    vec4 sunColorAmbient;
} terrain;

layout(set=0, binding=0, std430) readonly buffer HeightmapBuffer
{
    float heights[];
} heightmapBuffer;

struct TerrainInstance
{
    vec3 position;
    uint packedMetadata;
};

layout(set=0, binding=1, std430) readonly buffer InstanceBuffer
{
    TerrainInstance instanceData[];
} instanceBuffer;

layout(location = 0) in vec2 inLocalCoord;
layout(location = 1) in vec2 inSampleCoord;

layout(location = 0) out vec3 fragColor;

const uint kHeightmapResolution = 257u;
const float kHeightmapIntervalCount = 256.0;
const float kMinimumQuadSize = 256.0;
const float kHeightAmplitude = 1000.0;
const float kBaseHeight = 0.0;

float sampleHeight(uint sliceIndex, ivec2 sampleCoord)
{
    ivec2 clampedCoord = clamp(sampleCoord, ivec2(0), ivec2(256));
    uint linearIndex =
        (sliceIndex * kHeightmapResolution * kHeightmapResolution) +
        (uint(clampedCoord.y) * kHeightmapResolution) +
        uint(clampedCoord.x);
    return heightmapBuffer.heights[linearIndex];
}

vec3 computeNormal(uint sliceIndex, ivec2 sampleCoord, float sampleSpacing)
{
    ivec2 leftCoord = ivec2(max(sampleCoord.x - 1, 0), sampleCoord.y);
    ivec2 rightCoord = ivec2(min(sampleCoord.x + 1, 256), sampleCoord.y);
    ivec2 downCoord = ivec2(sampleCoord.x, max(sampleCoord.y - 1, 0));
    ivec2 upCoord = ivec2(sampleCoord.x, min(sampleCoord.y + 1, 256));

    float hL = sampleHeight(sliceIndex, leftCoord);
    float hR = sampleHeight(sliceIndex, rightCoord);
    float hD = sampleHeight(sliceIndex, downCoord);
    float hU = sampleHeight(sliceIndex, upCoord);

    float deltaX = sampleSpacing * float(max(rightCoord.x - leftCoord.x, 1));
    float deltaZ = sampleSpacing * float(max(upCoord.y - downCoord.y, 1));

    vec3 tangentX = vec3(deltaX, hR - hL, 0.0);
    vec3 tangentZ = vec3(0.0, hU - hD, deltaZ);
    return normalize(cross(tangentZ, tangentX));
}

vec3 terrainAlbedo(float height)
{
    float normalizedHeight = clamp((height - kBaseHeight) / (kHeightAmplitude * 1.8), 0.0, 1.0);
    vec3 lowland = vec3(0.14, 0.34, 0.16);
    vec3 highland = vec3(0.46, 0.40, 0.31);
    return mix(lowland, highland, normalizedHeight);
}

void main()
{
    TerrainInstance instance = instanceBuffer.instanceData[gl_InstanceIndex];
    uint sliceIndex = instance.packedMetadata & 0xFFFFu;
    uint scalePow = (instance.packedMetadata >> 16u) & 0xFFu;
    float leafSize = kMinimumQuadSize * exp2(float(scalePow));
    float sampleSpacing = leafSize / kHeightmapIntervalCount;
    vec2 localOffset = vec2(inLocalCoord.x * sampleSpacing, inLocalCoord.y * sampleSpacing);
    ivec2 sampleCoord = ivec2(inSampleCoord);

    float height = sampleHeight(sliceIndex, sampleCoord);

    vec3 worldPosition = vec3(
        instance.position.x + localOffset.x,
        instance.position.y + height,
        instance.position.z + localOffset.y
    );

    vec3 normal = computeNormal(sliceIndex, sampleCoord, sampleSpacing);
    vec3 sunDirection = normalize(terrain.sunDirectionIntensity.xyz);
    float diffuse = max(dot(normal, sunDirection), 0.0) * terrain.sunDirectionIntensity.w;
    vec3 ambient = terrain.sunColorAmbient.rgb * terrain.sunColorAmbient.a;
    vec3 litColor = terrainAlbedo(height) * (ambient + (terrain.sunColorAmbient.rgb * diffuse));

    gl_Position = terrain.viewProjection * vec4(worldPosition, 1.0);
    fragColor = litColor;
}
