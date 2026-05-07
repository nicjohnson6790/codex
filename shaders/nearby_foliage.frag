#version 450

layout(set=3, binding=0) uniform NearbyFoliageMaterialUniforms
{
    vec4 sunDirectionIntensity;
    vec4 sunColorAmbient;
} foliageMaterial;

layout(set=2, binding=0) uniform sampler2DArray baseColorTextureArray;
layout(set=2, binding=1) uniform sampler2DArray normalTextureArray;

struct NearbyMaterialGpu
{
    uvec4 layersAndFlags;
    vec4 params;
};

layout(set=2, binding=2, std430) readonly buffer NearbyMaterialBuffer
{
    NearbyMaterialGpu materials[];
} materialBuffer;

layout(location = 0) in vec2 fragUv0;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragTangent;
layout(location = 3) in vec3 fragBitangent;
layout(location = 4) flat in uint fragMaterialIndex;

layout(location = 0) out vec4 outColor;

void main()
{
    NearbyMaterialGpu material = materialBuffer.materials[fragMaterialIndex];
    float alphaCutoff = material.params.x;
    float baseColorLayer = float(material.layersAndFlags.x);
    float normalLayer = float(material.layersAndFlags.y);

    vec4 albedoSample = texture(baseColorTextureArray, vec3(fragUv0, baseColorLayer));
    if (albedoSample.a < alphaCutoff)
    {
        discard;
    }

    vec3 tangentNormal = texture(normalTextureArray, vec3(fragUv0, normalLayer)).xyz * 2.0 - 1.0;
    if (!gl_FrontFacing)
    {
        tangentNormal.xy *= -1.0;
    }
    mat3 tbn = mat3(
        normalize(fragTangent),
        normalize(fragBitangent),
        normalize(fragNormal));
    vec3 normal = normalize(tbn * tangentNormal);
    if (!gl_FrontFacing)
    {
        normal = -normal;
    }
    vec3 sunDirection = normalize(foliageMaterial.sunDirectionIntensity.xyz);
    float diffuse = max(dot(normal, sunDirection), 0.0) * foliageMaterial.sunDirectionIntensity.w;
    float backLight = max(dot(-normal, sunDirection), 0.0) * 0.35;
    vec3 ambient = foliageMaterial.sunColorAmbient.rgb * foliageMaterial.sunColorAmbient.a;
    vec3 litColor = albedoSample.rgb * (ambient + (foliageMaterial.sunColorAmbient.rgb * (diffuse + backLight)));

    outColor = vec4(litColor, albedoSample.a);
}
