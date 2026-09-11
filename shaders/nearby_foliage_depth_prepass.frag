#version 450

layout(set=2, binding=0) uniform sampler2DArray baseColorTextureArray;

struct NearbyMaterialGpu
{
    uvec4 layers0;
    uvec4 layers1;
    vec4 params;
};

layout(set=2, binding=1, std430) readonly buffer NearbyMaterialBuffer
{
    NearbyMaterialGpu materials[];
} materialBuffer;

layout(location = 0) in vec2 fragUv0;
layout(location = 5) flat in uint fragMaterialIndex;
layout(location = 4) in vec3 fragViewPosition;
layout(location = 6) flat in uint fragLodIndex;

layout(location = 0) out vec4 outColor;

void main()
{
    if(fragLodIndex==2u) {
        float alpha=clamp((105.0-length(fragViewPosition.xz))/5.0,0.0,1.0);
        float noise=fract(52.9829189*fract(dot(gl_FragCoord.xy,vec2(0.06711056,0.00583715))));
        if(alpha<=0.0 || (alpha<1.0 && noise>alpha)) discard;
    }
    NearbyMaterialGpu material = materialBuffer.materials[fragMaterialIndex];
    float alphaCutoff = material.params.x;
    float baseColorLayer = float(material.layers0.x);
    vec4 albedoSample = texture(baseColorTextureArray, vec3(fragUv0, baseColorLayer));
    if (albedoSample.a < alphaCutoff)
    {
        discard;
    }

    outColor = vec4(0.0);
}
