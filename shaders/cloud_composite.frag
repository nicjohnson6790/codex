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
    vec2 ndc=cloudPixelToNdc(gl_FragCoord.xy,vec2(u.extent.xy));
    vec4 rayBase=u.inverseViewProjection*vec4(ndc,0.0,1.0);
    vec4 scenePosition=rayBase+u.inverseViewProjection[2]*sceneDepth;
    float sceneDistance=sceneDepth>0.0 ? length(scenePosition.xyz/max(scenePosition.w,1e-20)) : 1e30;
    ivec2 last=ivec2(u.extent.xy)-ivec2(1);
    float left=texelFetch(sceneDepthTexture,max(pixel-ivec2(1,0),ivec2(0)),0).r;
    float right=texelFetch(sceneDepthTexture,min(pixel+ivec2(1,0),last),0).r;
    float up=texelFetch(sceneDepthTexture,max(pixel-ivec2(0,1),ivec2(0)),0).r;
    float down=texelFetch(sceneDepthTexture,min(pixel+ivec2(0,1),last),0).r;
    // Use the smoother side of each axis rather than the jump across a silhouette.
    // Three full pixels cover the bilinear taps plus their 2x2 depth footprints.
    float depthSlope=min(abs(left-sceneDepth),abs(right-sceneDepth))
        +min(abs(up-sceneDepth),abs(down-sceneDepth));
    float slopeDepth=max(sceneDepth-3.0*depthSlope,sceneDepth*0.5);
    vec4 slopePosition=rayBase+u.inverseViewProjection[2]*slopeDepth;
    float footprintDifference=sceneDepth>0.0
        ? max(0.0,length(slopePosition.xyz/max(slopePosition.w,1e-20))-sceneDistance) : 0.0;
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
        // Compare both depths on this full-resolution ray so ray angle alone
        // cannot create a mismatch between the low and high resolution grids.
        vec4 marchPosition=rayBase+u.inverseViewProjection[2]*marchDepth;
        float marchDistance=marchDepth>0.0 ? length(marchPosition.xyz/max(marchPosition.w,1e-20)) : 1e30;
        if(cloudDepthAccept(sceneDepth,marchDepth,sceneDistance,marchDistance,footprintDifference))
            result+=weight*texelFetch(cloudColorTexture,tap,0);
        // Rejected weight stays transparent: retain the atmosphere-treated scene.
    }
    outColor=result;
}
