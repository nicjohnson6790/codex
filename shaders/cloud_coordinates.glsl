// SDL presents a top-left framebuffer while clip-space +Y points upward.
// Vulkan implements this with a negative-height viewport.
vec2 cloudPixelToNdc(vec2 pixelCenter,vec2 fullExtent)
{
    vec2 uv=pixelCenter/fullExtent;
    return vec2(uv.x*2.0f-1.0f,1.0f-uv.y*2.0f);
}

// Reversed depth: zero is unobstructed sky; the minimum reaches farthest.
float cloudFarthestDepth(vec4 depths)
{
    return min(min(depths.x,depths.y),min(depths.z,depths.w));
}

float cloudDepthTolerance(float sceneDistance)
{
    // Metres: neighboring rays on continuous ground need geometric tolerance,
    // not just floating-point epsilon in nonlinear reversed depth.
    return max(1.0f,sceneDistance*0.005f);
}

float cloudSlopeTolerance(float sceneDistance,float footprintDifference)
{
    // Bound slope-driven relaxation at silhouettes; 0.5% remains the floor.
    return max(cloudDepthTolerance(sceneDistance),min(max(footprintDifference,0.0f),sceneDistance*0.05f));
}

bool cloudDepthAccept(float sceneDepth,float marchDepth,float sceneDistance,float marchDistance,float footprintDifference)
{
    if(sceneDepth<=0.0f) return true;
    // A sky-bounded march must never bleed over finite foreground geometry.
    if(marchDepth<=0.0f) return false;
    return marchDistance <= sceneDistance+cloudSlopeTolerance(sceneDistance,footprintDifference);
}

bool cloudDepthAccept(float sceneDepth,float marchDepth,float sceneDistance,float marchDistance)
{
    return cloudDepthAccept(sceneDepth,marchDepth,sceneDistance,marchDistance,0.0f);
}

vec2 cloudMarchPixelCenter(ivec2 pixel,ivec2 fullSize,bool quarter)
{
    ivec2 base=pixel*(quarter ? 2 : 1);
    ivec2 size=quarter ? min(ivec2(2),fullSize-base) : ivec2(1);
    return vec2(base)+vec2(size)*0.5f;
}

float cloudReconstructionAxis(float pixelCenter,int fullSize)
{
    // An odd final block is one pixel wide, so its center is only 1.5 pixels
    // from the preceding block's center instead of the usual two.
    if(fullSize>1 && (fullSize%2)!=0 && pixelCenter>float(fullSize)-2.0f)
        return float(fullSize/2-1)+(pixelCenter-(float(fullSize)-2.0f))/1.5f;
    return pixelCenter*0.5f-0.5f;
}
