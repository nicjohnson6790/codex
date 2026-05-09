#version 450

layout(set=2, binding=0) uniform sampler2DArray imposterColorTextureArray;

layout(location = 0) in vec2 fragUv0;
layout(location = 1) flat in uint fragLayerIndex0;
layout(location = 2) flat in uint fragLayerIndex1;
layout(location = 3) in float fragYawBlend;

layout(location = 0) out vec4 outColor;

void main()
{
    vec4 colorSample0 = texture(imposterColorTextureArray, vec3(fragUv0, float(fragLayerIndex0)));
    vec4 colorSample1 = texture(imposterColorTextureArray, vec3(fragUv0, float(fragLayerIndex1)));
    vec4 colorSample = mix(colorSample0, colorSample1, fragYawBlend);
    if (colorSample.a < 0.5)
    {
        discard;
    }

    outColor = vec4(0.0);
}
