// Fullscreen-only state. The shared 304-byte field used by water/probes is unchanged.
#include "atmosphere.glsl"
#include "cloud_integration.glsl"
#include "cloud_coordinates.glsl"
layout(set=3,binding=0) uniform CloudUniforms
{
    mat4 inverseViewProjection;
    CloudSamplingState cloud;
    uvec4 extent;
} u;

vec3 cloudScreenPosition(vec2 ndc,float depth)
{
    vec4 p=u.inverseViewProjection*vec4(ndc,depth,1.0);
    return p.xyz/max(p.w,1e-20);
}
vec2 cloudScreenNdc(vec2 pixelCenter)
{
    return cloudPixelToNdc(pixelCenter,vec2(u.extent.xy));
}
float cloudScreenLimit(vec2 ndc,float depth)
{
    return depth>0.0 ? length(cloudScreenPosition(ndc,depth)) : 1e30;
}
