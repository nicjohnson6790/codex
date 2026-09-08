#version 450
#extension GL_GOOGLE_include_directive : require
#include "atmosphere.glsl"
#include "cloud_density.glsl"
#include "cloud_optics.glsl"
#include "cloud_slab.glsl"
layout(location=0) in vec2 fragNdc;
layout(location=0) out vec4 outColor;
layout(set=2,binding=0) uniform sampler2D depthTexture;
layout(set=2,binding=1) uniform sampler2D coverageTexture;
layout(set=2,binding=2) uniform sampler3D noiseTexture;
layout(set=3,binding=0) uniform CloudUniforms
{
    mat4 inverseViewProjection;
    CloudDensityField field;
    vec4 sun;
    vec4 atmosphere; // camera altitude, atmosphere top, ambient strength, sky exposure
    AtmosphereOptics optics;
    vec4 scattering; // extinction, g, lobe weight, octave count
    vec4 powder; // strength, angular power, view steps, sun steps
    vec4 march; // maximum distance, termination threshold
    vec4 octaves;
} u;
vec3 reconstruct(float depth) { vec4 p=u.inverseViewProjection*vec4(fragNdc,depth,1.0); return p.xyz/max(p.w,1e-20); }
float density(vec3 p) { return sampleCloudDensity(p,u.field,coverageTexture,noiseTexture); }
void main()
{
    float depth=texelFetch(depthTexture,ivec2(gl_FragCoord.xy),0).r;
    vec3 ray=normalize(reconstruct(1.0));
    float end=depth>0.0 ? min(length(reconstruct(depth)),u.march.x) : u.march.x;
    float base=u.field.layer.x, top=base+u.field.layer.y;
    vec2 interval=cloudSlabInterval(-base,ray.y,u.field.layer.y,end);
    float begin=interval.x; end=interval.y;
    if(end<=begin) discard;
    int count=clamp(int(u.powder.z),8,128), lightCount=clamp(int(u.powder.w),1,32);
    float ds=(end-begin)/float(count), viewT=1.0;
    vec3 radiance=vec3(0.0);
    float mu=dot(ray,u.sun.xyz);
    for(int i=0;i<count && viewT>u.march.y;++i)
    {
        float distance=begin+(float(i)+0.5)*ds;
        vec3 p=ray*distance;
        float d=density(p);
        if(d<=0.0) continue;
        float tauSun=0.0;
        if(u.sun.y>0.0)
        {
            // The actual slab exit, including near-horizon long paths. One march for all octaves.
            float lightLength=(top-p.y)/u.sun.y;
            float lightStep=lightLength/float(lightCount);
            for(int j=0;j<lightCount;++j)
                tauSun+=density(p+u.sun.xyz*((float(j)+0.5)*lightStep))*u.scattering.x*lightStep;
        }
        float energy=cloudMultipleScattering(tauSun,mu,u.scattering.y,u.scattering.z,
            clamp(int(u.scattering.w),1,12),u.octaves.xyz)*cloudPowder(tauSun,mu,u.powder.x,u.powder.y);
        vec3 sunlight=airSunTransmission(p.y+u.atmosphere.x,u.sun.xyz,u.atmosphere.y,u.optics)*u.optics.solar.rgb*u.optics.solar.w;
        vec3 lighting=sunlight*energy+u.atmosphere.z*sqrt(max(sunlight,vec3(0.0)))*vec3(0.65,0.78,1.0);
        vec3 airT,airS;
        evaluateAtmosphere(u.atmosphere.x,ray,distance,u.atmosphere.y,u.sun.xyz,u.optics,true,airT,airS);
        float stepT=exp(-d*u.scattering.x*ds);
        radiance+=viewT*(airS+airT*lighting)*(1.0-stepT);
        viewT*=stepT;
    }
    float opacity=1.0-viewT;
    // Integrate linear radiance first. Map the effective cloud source once into the
    // same display-referred viewport as the sky, then premultiply for compositing.
    vec3 displayed=opacity>1e-6 ? displaySkyRadiance(vec3(0.0),vec3(1.0),radiance/opacity,u.optics) : vec3(0.0);
    outColor=vec4(displayed*opacity,opacity);
}
