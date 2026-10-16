#version 450
#extension GL_GOOGLE_include_directive : require
#include "cloud_coordinates.glsl"
layout(location=0) out vec4 outColor;
layout(set=2,binding=0) uniform sampler2D cloudColorTexture;
layout(set=2,binding=1) uniform sampler2D cloudMarchDepthTexture;
layout(set=2,binding=2) uniform sampler2D sceneDepthTexture;
layout(set=3,binding=0) uniform CloudCompositeUniforms
{
    mat4 inverseViewProjection;
    vec4 unusedCloudState[19]; // Preserve the shared CPU fullscreen block layout.
    uvec4 extent;
} u;
void main()
{
    ivec2 pixel=ivec2(gl_FragCoord.xy);
    if(all(equal(u.extent.xy,u.extent.zw)))
    {
        outColor=texelFetch(cloudColorTexture,pixel,0);
        return;
    }
    float sceneDepth=texelFetch(sceneDepthTexture,pixel,0).r;
    vec2 lowPosition=vec2(cloudReconstructionAxis(gl_FragCoord.x,int(u.extent.x)),
        cloudReconstructionAxis(gl_FragCoord.y,int(u.extent.y)));
    ivec2 base=ivec2(floor(lowPosition));
    vec2 fraction=fract(lowPosition);
    vec4 result=vec4(0.0);
    for(int y=0;y<2;++y) for(int x=0;x<2;++x)
    {
        ivec2 tap=clamp(base+ivec2(x,y),ivec2(0),ivec2(u.extent.zw)-ivec2(1));
        float weight=(x==0 ? 1.0-fraction.x : fraction.x)*(y==0 ? 1.0-fraction.y : fraction.y);
        float marchDepth=texelFetch(cloudMarchDepthTexture,tap,0).r;
        if(cloudDepthAccept(sceneDepth,marchDepth))
            result+=weight*texelFetch(cloudColorTexture,tap,0);
        // Rejected weight stays transparent: retain the atmosphere-treated scene.
    }
    outColor=result;
}
