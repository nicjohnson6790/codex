#version 450
#extension GL_GOOGLE_include_directive : require
#include "atmosphere.glsl"
#include "cloud_integration.glsl"
layout(location=0) in vec2 fragNdc;
layout(location=0) out vec4 outColor;
layout(set=2,binding=0) uniform sampler2D depthTexture;
layout(set=2,binding=1) uniform sampler2D coverageTexture;
layout(set=2,binding=2) uniform sampler3D noiseTexture;
layout(set=3,binding=0) uniform CloudUniforms
{
    mat4 inverseViewProjection;
    CloudSamplingState cloud;
} u;
vec3 reconstruct(float depth) { vec4 p=u.inverseViewProjection*vec4(fragNdc,depth,1.0); return p.xyz/max(p.w,1e-20); }
void main()
{
    float depth=texelFetch(depthTexture,ivec2(gl_FragCoord.xy),0).r;
    vec3 ray=normalize(reconstruct(1.0));
    float end=depth>0.0 ? length(reconstruct(depth)) : 1e30;
    outColor=integrateCloudRay(vec3(0.0),ray,end,u.cloud,coverageTexture,noiseTexture);
}
