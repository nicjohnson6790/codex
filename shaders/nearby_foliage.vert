#version 450
#extension GL_ARB_shader_draw_parameters : require

layout(set=1, binding=0) uniform NearbyFoliageUniforms
{
    mat4 viewProjection;
} foliage;

struct NearbyDrawInstance
{
    vec4 pageOriginAndSlice;
    vec4 localOffsetAndMesh;
};

layout(set=0, binding=0, std430) readonly buffer NearbyDrawInstanceBuffer
{
    NearbyDrawInstance instances[];
} drawInstanceBuffer;

layout(set=0, binding=1, std430) readonly buffer HeightmapBuffer
{
    float heights[];
} heightmapBuffer;

layout(location = 0) in float inEndpoint;

layout(location = 0) out vec3 fragColor;

const uint kHeightmapResolution = 259u;
const float kHeightmapLeafIntervalCount = 256.0;

float sampleHeight(uint sliceIndex, ivec2 sampleCoord)
{
    ivec2 clampedCoord = clamp(sampleCoord, ivec2(0), ivec2(int(kHeightmapResolution - 1u)));
    uint linearIndex =
        (sliceIndex * kHeightmapResolution * kHeightmapResolution) +
        (uint(clampedCoord.y) * kHeightmapResolution) +
        uint(clampedCoord.x);
    return heightmapBuffer.heights[linearIndex];
}

float sampleHeightBilinear(uint sliceIndex, vec2 normalizedCoord)
{
    vec2 clampedCoord = clamp(normalizedCoord, vec2(0.0), vec2(1.0));
    vec2 sampleCoord = vec2(1.0) + (clampedCoord * kHeightmapLeafIntervalCount);
    ivec2 baseCoord = ivec2(floor(sampleCoord));
    vec2 fracCoord = fract(sampleCoord);

    float h00 = sampleHeight(sliceIndex, baseCoord);
    float h10 = sampleHeight(sliceIndex, baseCoord + ivec2(1, 0));
    float h01 = sampleHeight(sliceIndex, baseCoord + ivec2(0, 1));
    float h11 = sampleHeight(sliceIndex, baseCoord + ivec2(1, 1));

    float row0 = mix(h00, h10, fracCoord.x);
    float row1 = mix(h01, h11, fracCoord.x);
    return mix(row0, row1, fracCoord.y);
}

void main()
{
    NearbyDrawInstance instance = drawInstanceBuffer.instances[gl_InstanceIndex];
    vec3 pageOrigin = instance.pageOriginAndSlice.xyz;
    uint terrainSliceIndex = uint(instance.pageOriginAndSlice.w + 0.5);
    vec2 localOffset = instance.localOffsetAndMesh.xy;
    uint meshId = uint(instance.localOffsetAndMesh.w + 0.5);
    float terrainHeight = sampleHeightBilinear(terrainSliceIndex, localOffset / 256.0);
    float markerHeight =
        meshId == 0u ? 8.0 :
        (meshId == 1u ? 14.0 : 22.0);
    vec3 worldPosition = vec3(
        pageOrigin.x + localOffset.x,
        pageOrigin.y + terrainHeight + (inEndpoint * markerHeight),
        pageOrigin.z + localOffset.y);
    gl_Position = foliage.viewProjection * vec4(worldPosition, 1.0);
    fragColor =
        meshId == 0u ? vec3(1.0, 0.05, 0.9) :
        (meshId == 1u ? vec3(1.0, 0.1, 0.45) : vec3(1.0, 0.35, 0.1));
}
