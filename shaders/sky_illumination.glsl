#include "sky_illumination_math.glsl"
// Reusable read-only field. A receiver region is a conservative candidate-list
// hint, not probe identity; other renderers can use the covering terrain region.
struct SkyProbeRegion { vec4 domain; vec4 source; uvec4 lookup; };
#ifndef SKY_PROBE_COEFFICIENT_BINDING
#define SKY_PROBE_COEFFICIENT_BINDING 9
#endif
#ifndef SKY_PROBE_REGION_BINDING
#define SKY_PROBE_REGION_BINDING 10
#endif
layout(set=2,binding=SKY_PROBE_COEFFICIENT_BINDING,std430) readonly buffer SkyProbeCoefficients { vec4 values[]; } skyProbeCoefficients;
layout(set=2,binding=SKY_PROBE_REGION_BINDING,std430) readonly buffer SkyProbeRegions
{
    uvec4 header; // region count, fallback region, reserved
    SkyProbeRegion regions[512];
    uint candidates[];
} skyProbeField;
vec3 skyProbeGrid(SkyProbeRegion region,vec3 p,vec3 normal)
{
    vec2 coord=skyProbeGridCoordinate((p.xz-region.source.xy)/region.source.z);
    uvec2 a=uvec2(floor(coord)), b=min(a+uvec2(1),uvec2(3));
    vec2 f=fract(coord);
    uint base=region.lookup.x*16u*9u;
    vec3 result=vec3(0.0);
    for(uint coefficient=0u;coefficient<9u;++coefficient)
    {
        vec3 low=mix(skyProbeCoefficients.values[base+(a.y*4u+a.x)*9u+coefficient].rgb,
                     skyProbeCoefficients.values[base+(a.y*4u+b.x)*9u+coefficient].rgb,f.x);
        vec3 high=mix(skyProbeCoefficients.values[base+(b.y*4u+a.x)*9u+coefficient].rgb,
                      skyProbeCoefficients.values[base+(b.y*4u+b.x)*9u+coefficient].rgb,f.x);
        result+=mix(low,high,f.y)*skyProbeBasis(int(coefficient),normal);
    }
    return max(result,vec3(0.0));
}
vec3 sampleSkyIllumination(vec3 p,vec3 normal,uint receiverRegion)
{
    if(receiverRegion>=skyProbeField.header.x || skyProbeField.header.y==65535u) return vec3(0.0);
    SkyProbeRegion receiver=skyProbeField.regions[receiverRegion];
    vec3 irradiance=vec3(0.0); float total=0.0;
    for(uint i=0u;i<receiver.lookup.z;++i)
    {
        SkyProbeRegion region=skyProbeField.regions[skyProbeField.candidates[receiver.lookup.y+i]];
        float weight=skyProbeRegionWeight(p.xz,region.domain);
        if(weight<=0.0) continue;
        irradiance+=weight*skyProbeGrid(region,p,normal); total+=weight;
    }
    vec3 fallback=skyProbeGrid(skyProbeField.regions[skyProbeField.header.y],p,normal);
    return mix(fallback,irradiance/max(total,1e-8),min(total,1.0));
}
