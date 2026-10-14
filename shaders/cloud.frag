#version 450
#extension GL_GOOGLE_include_directive : require
#include "cloud_screen.glsl"
layout(location=0) out vec4 outColor;
layout(set=2,binding=0) uniform sampler2D depthTexture;
layout(set=2,binding=1) uniform sampler2D coverageTexture;
layout(set=2,binding=2) uniform sampler3D noiseTexture;
void main()
{
    vec2 uv=gl_FragCoord.xy/vec2(u.extent.zw);
    vec2 ndc=cloudScreenNdc(uv*vec2(u.extent.xy));
    vec3 ray=normalize(cloudScreenPosition(ndc,1.0));
    float depth=textureLod(depthTexture,uv,0.0).r;
    outColor=integrateCloudRay(vec3(0.0),ray,cloudScreenLimit(ndc,depth),
        u.cloud,coverageTexture,noiseTexture);
}
