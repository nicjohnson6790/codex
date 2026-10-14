#version 450
layout(location=0) out vec4 outColor;
layout(set=2,binding=0) uniform sampler2D cloudColorTexture;
layout(set=3,binding=0) uniform CloudCompositeUniforms
{
    mat4 inverseViewProjection;
    vec4 unusedCloudState[19]; // Preserve the shared CPU fullscreen block layout.
    uvec4 extent;
} u;
void main()
{
    outColor=textureLod(cloudColorTexture,gl_FragCoord.xy/vec2(u.extent.xy),0.0);
}
