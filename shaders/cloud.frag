#version 450
#extension GL_GOOGLE_include_directive : require
#include "cloud_screen.glsl"
layout(location=0) out vec4 outColor;
layout(location=1) out float outMarchDepth;
layout(set=2,binding=0) uniform sampler2D depthTexture;
layout(set=2,binding=1) uniform sampler2D coverageTexture;
layout(set=2,binding=2) uniform sampler3D noiseTexture;
void main()
{
    bool quarter=any(notEqual(u.extent.xy,u.extent.zw));
    ivec2 fullSize=ivec2(u.extent.xy);
    ivec2 base=ivec2(gl_FragCoord.xy)*(quarter ? 2 : 1);
    ivec2 last=fullSize-ivec2(1);
    float depth=texelFetch(depthTexture,base,0).r;
    if(quarter)
        depth=cloudFarthestDepth(vec4(depth,
            texelFetch(depthTexture,min(base+ivec2(1,0),last),0).r,
            texelFetch(depthTexture,min(base+ivec2(0,1),last),0).r,
            texelFetch(depthTexture,min(base+ivec2(1,1),last),0).r));
    vec2 pixelCenter=cloudMarchPixelCenter(ivec2(gl_FragCoord.xy),fullSize,quarter);
    vec2 ndc=cloudScreenNdc(pixelCenter);
    vec3 ray=normalize(cloudScreenPosition(ndc,1.0));
    outMarchDepth=depth;
    outColor=integrateCloudRay(vec3(0.0),ray,cloudScreenLimit(ndc,depth),
        u.cloud,coverageTexture,noiseTexture);
}
