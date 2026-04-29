#version 450

layout(set=1, binding=0) uniform WaterUniforms
{
    mat4 viewProjection;
    vec4 cameraAndTime;
    vec4 waterParams;
    vec4 sunDirectionIntensity;
    vec4 sunColorAmbient;
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

void main()
{
    WaterInstance instance = instanceBuffer.instanceData[gl_InstanceIndex];
    float leafSize = instance.leafParams.x;
    float waterLevel = instance.leafParams.y;
    uint waterMeshLod = (instance.packedMetadata >> 8u) & 0xFFu;

    vec2 localMeters = inLocalCoord * leafSize;
    vec3 position = vec3(
        instance.position.x + localMeters.x,
        instance.position.y + waterLevel,
        instance.position.z + localMeters.y);

    fragNormal = vec3(0.0, 1.0, 0.0);
    fragWorldPosition = position;
    fragLodBlend = clamp(float(waterMeshLod) / 4.0, 0.0, 1.0);
    gl_Position = water.viewProjection * vec4(position, 1.0);
}
