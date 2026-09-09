#include "cloud_density.glsl"
#include "cloud_optics.glsl"
#include "cloud_slab.glsl"

struct CloudSamplingState
{
    CloudDensityField field;
    vec4 sun;
    vec4 atmosphere; // render-origin altitude, atmosphere top, cloud ambient, reserved
    AtmosphereOptics optics;
    vec4 scattering; // extinction, g, lobe weight, octave count
    vec4 powder; // strength, angular power, view steps, sun steps
    vec4 march; // maximum distance, termination threshold, enabled/ready, unused
    vec4 octaves;
};

// Returns linear premultiplied cloud radiance and opacity. Only the caller clips scene depth.
vec4 integrateCloudRay(vec3 origin,vec3 ray,float end,CloudSamplingState u,
    sampler2D coverageTexture,sampler3D noiseTexture)
{
    float base=u.field.layer.x, top=base+u.field.layer.y;
    vec2 interval=cloudSlabInterval(origin.y-base,ray.y,u.field.layer.y,end);
    float begin=interval.x; end=interval.y;
    if(end<=begin) return vec4(0.0);
    int count=clamp(int(u.powder.z),1,128), lightCount=clamp(int(u.powder.w),1,32);
    float ds=(end-begin)/float(count), viewT=1.0;
    vec3 radiance=vec3(0.0);
    float mu=dot(ray,u.sun.xyz);
    for(int i=0;i<count && viewT>u.march.y;++i)
    {
        float distance=begin+(float(i)+0.5)*ds;
        vec3 p=origin+ray*distance;
        float d=sampleCloudDensity(p,u.field,coverageTexture,noiseTexture);
        if(d<=0.0) continue;
        float tauSun=0.0;
        if(u.sun.y>0.0)
        {
            // The actual slab exit, including near-horizon long paths. One march for all octaves.
            float lightLength=(top-p.y)/u.sun.y;
            float lightStep=lightLength/float(lightCount);
            for(int j=0;j<lightCount;++j)
                tauSun+=sampleCloudDensity(p+u.sun.xyz*((float(j)+0.5)*lightStep),u.field,coverageTexture,noiseTexture)*u.scattering.x*lightStep;
        }
        float energy=cloudMultipleScattering(tauSun,mu,u.scattering.y,u.scattering.z,
            clamp(int(u.scattering.w),1,12),u.octaves.xyz)*cloudPowder(tauSun,mu,u.powder.x,u.powder.y);
        vec3 sunlight=airSunTransmission(p.y+u.atmosphere.x,u.sun.xyz,u.atmosphere.y,u.optics)*u.optics.solar.rgb*u.optics.solar.w;
        vec3 lighting=sunlight*energy+u.atmosphere.z*sqrt(max(sunlight,vec3(0.0)))*vec3(0.65,0.78,1.0);
        lighting *= u.optics.radianceScales.x; // After the complete lighting expression, including sqrt ambient.
        vec3 airT,airS;
        evaluateAtmosphere(u.atmosphere.x+origin.y,ray,distance,u.atmosphere.y,u.sun.xyz,u.optics,true,airT,airS);
        float stepT=exp(-d*u.scattering.x*ds);
        radiance+=viewT*(airS+airT*lighting)*(1.0-stepT);
        viewT*=stepT;
    }
    float opacity=1.0-viewT;
    return vec4(radiance,opacity);
}
