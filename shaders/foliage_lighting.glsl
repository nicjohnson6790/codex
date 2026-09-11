#include "atmosphere.glsl"
#include "cloud_density.glsl"
#include "cloud_slab.glsl"
#include "cloud_shadow.glsl"
#include "terrain_lighting.glsl"
#include "sky_illumination.glsl"
layout(set=3,binding=1) uniform FoliageLightingUniforms {
    CloudDensityField field;
    vec4 params;
    AtmosphereOptics optics;
    vec4 atmosphere;
    vec4 solar;
} foliageLight;
layout(set=2,binding=FOLIAGE_CLOUD_BINDING) uniform sampler2D foliageCloudCoverage;
layout(set=2,binding=FOLIAGE_NOISE_BINDING) uniform sampler3D foliageCloudNoise;
vec3 foliageSun(vec3 p,vec3 direction)
{
    float visibility=terrainSolarVisibility(direction.y,foliageLight.solar.x);
    if(visibility<=0.0) return vec3(0);
    return surfaceSunIrradiance(foliageLight.atmosphere.w+p.y,direction,
        foliageLight.atmosphere.y,foliageLight.optics)*visibility*
        terrainCloudTransmission(p,direction,foliageLight.field,foliageLight.params,foliageCloudCoverage,foliageCloudNoise);
}
vec3 foliageSky(vec3 p,vec3 normal,uint hint)
{
    // A tree or canopy footprint can cross its receiver tile. Reassociate at
    // the sampling point rather than trusting an unrelated frame-local hint.
    bool covered=false;
    if(hint<skyProbeField.header.x) {
        vec4 d=skyProbeField.regions[hint].domain;
        covered=all(greaterThanEqual(p.xz,d.xy))&&all(lessThanEqual(p.xz,d.xy+d.zz));
    }
    if(!covered) for(uint i=0u;i<skyProbeField.header.x;++i) {
        vec4 d=skyProbeField.regions[i].domain;
        if(all(greaterThanEqual(p.xz,d.xy))&&all(lessThanEqual(p.xz,d.xy+d.zz))) {hint=i;covered=true;break;}
    }
    if(!covered) hint=skyProbeField.header.y;
    return sampleSkyIllumination(p,normal,hint);
}
