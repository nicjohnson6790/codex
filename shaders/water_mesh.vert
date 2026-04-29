#version 450

layout(set=1, binding=0) uniform WaterUniforms
{
    mat4 viewProjection;
    vec4 cameraAndTime;
    vec4 waterParams;
    vec4 sunDirectionIntensity;
    vec4 sunColorAmbient;
    vec4 debugParams;
    vec4 cascadeWorldSizesA;
    vec4 cascadeWorldSizesB;
} water;

layout(set=0, binding=0) uniform sampler2DArray displacementTexture;

struct WaterInstance
{
    vec3 position;
    uint packedMetadata;
    vec4 leafParams;
};

layout(set=0, binding=1, std430) readonly buffer InstanceBuffer
{
    WaterInstance instanceData[];
} instanceBuffer;

layout(location = 0) in vec2 inLocalCoord;

layout(location = 0) out vec3 fragWorldPosition;
layout(location = 1) flat out uint fragBandMask;
layout(location = 2) out float fragLodBlend;

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
    WaterInstance instance = instanceBuffer.instanceData[gl_InstanceIndex];
    float leafSize = instance.leafParams.x;
    float waterLevel = instance.leafParams.y;
    uint bandMask = (instance.packedMetadata >> 16u) & 0xFFFFu;
    uint cascadeCount = uint(max(water.waterParams.w, 0.0));

    vec2 localMeters = inLocalCoord * leafSize;
    vec3 position = vec3(
        instance.position.x + localMeters.x,
        instance.position.y + waterLevel,
        instance.position.z + localMeters.y);
    vec2 worldXZ = water.cameraAndTime.xy + position.xz;

    vec3 displacement = vec3(0.0);
    for (uint cascadeIndex = 0u; cascadeIndex < cascadeCount; ++cascadeIndex)
    {
        if ((bandMask & (1u << cascadeIndex)) == 0u)
        {
            continue;
        }

        float worldSize = max(cascadeWorldSize(cascadeIndex), 1.0);
        vec2 uv = fract(worldXZ / worldSize);
        displacement += texture(displacementTexture, vec3(uv, float(cascadeIndex))).xyz;
    }

    position += displacement;

    fragWorldPosition = position;
    fragBandMask = bandMask;
    fragLodBlend = 0.0;
    gl_Position = water.viewProjection * vec4(position, 1.0);
}
