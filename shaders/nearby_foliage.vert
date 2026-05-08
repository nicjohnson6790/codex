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
    vec4 rotationAndReserved;
};

layout(set=0, binding=0, std430) readonly buffer NearbyDrawInstanceBuffer
{
    NearbyDrawInstance instances[];
} drawInstanceBuffer;

layout(set=0, binding=1, std430) readonly buffer HeightmapBuffer
{
    float heights[];
} heightmapBuffer;

struct NearbyDrawMetadata
{
    uvec4 instanceOffsetAndMaterial;
};

layout(set=0, binding=2, std430) readonly buffer NearbyDrawMetadataBuffer
{
    NearbyDrawMetadata draws[];
} drawMetadataBuffer;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUv0;

layout(location = 0) out vec2 fragUv0;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragTangent;
layout(location = 3) out vec3 fragBitangent;
layout(location = 4) out vec3 fragViewPosition;
layout(location = 5) flat out uint fragMaterialIndex;

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
    NearbyDrawMetadata draw = drawMetadataBuffer.draws[gl_DrawIDARB];
    uint instanceIndex = draw.instanceOffsetAndMaterial.x + uint(gl_InstanceIndex);
    NearbyDrawInstance instance = drawInstanceBuffer.instances[instanceIndex];
    vec3 pageOrigin = instance.pageOriginAndSlice.xyz;
    uint terrainSliceIndex = uint(instance.pageOriginAndSlice.w + 0.5);
    vec2 localOffset = instance.localOffsetAndMesh.xy;
    float rotationRadians = instance.rotationAndReserved.x;
    float terrainHeight = sampleHeightBilinear(terrainSliceIndex, localOffset / 256.0);
    float rotationCosine = cos(rotationRadians);
    float rotationSine = sin(rotationRadians);
    mat2 rotationMatrix = mat2(
        rotationCosine, -rotationSine,
        rotationSine, rotationCosine);
    vec2 rotatedPositionXz = rotationMatrix * inPosition.xz;
    vec2 rotatedNormalXz = rotationMatrix * inNormal.xz;
    vec2 rotatedTangentXz = rotationMatrix * inTangent.xz;

    vec3 worldPosition = vec3(
        pageOrigin.x + localOffset.x + rotatedPositionXz.x,
        pageOrigin.y + terrainHeight + inPosition.y,
        pageOrigin.z + localOffset.y + rotatedPositionXz.y);

    vec3 tangent = normalize(vec3(rotatedTangentXz.x, inTangent.y, rotatedTangentXz.y));
    vec3 normal = normalize(vec3(rotatedNormalXz.x, inNormal.y, rotatedNormalXz.y));
    vec3 bitangent = normalize(cross(normal, tangent)) * inTangent.w;

    gl_Position = foliage.viewProjection * vec4(worldPosition, 1.0);
    fragUv0 = vec2(inUv0.x, 1.0 - inUv0.y);
    fragNormal = normal;
    fragTangent = tangent;
    fragBitangent = bitangent;
    fragViewPosition = worldPosition;
    fragMaterialIndex = draw.instanceOffsetAndMaterial.y;
}
