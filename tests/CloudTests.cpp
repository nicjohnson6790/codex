#ifdef NDEBUG
#undef NDEBUG
#endif
#include "CloudManager.hpp"
#include "AppConfig.hpp"
#include "CloudSampling.hpp"
#include "PeriodicWorldPhase.hpp"
#include <glm/glm.hpp>
#include <cassert>
#include <iostream>
#include <limits>
#include "SkyIlluminationTests.hpp"

namespace Shader
{
using namespace glm;
// Float literals in GLSL are floats; use matching overloads for the CPU compilation.
inline float max(float a,double b) { return glm::max(a,float(b)); }
inline float clamp(float a,double b,double c) { return glm::clamp(a,float(b),float(c)); }
inline float mix(double a,float b,float c) { return glm::mix(float(a),b,c); }
inline float smoothstep(double a,double b,float c) { return glm::smoothstep(float(a),float(b),c); }
#include "../shaders/cloud_shape.glsl"
#include "../shaders/cloud_optics.glsl"
#include "../shaders/cloud_noise.glsl"
#include "../shaders/cloud_slab.glsl"
#include "../shaders/terrain_lighting.glsl"
// Constant density isolates the actual shadow march from texture generation.
struct CloudDensityField { vec4 layer, macro; };
using sampler2D = int;
using sampler3D = int;
float sampleCloudDensity(vec3, CloudDensityField field, sampler2D, sampler3D)
{
    return field.layer.w;
}
#include "../shaders/cloud_shadow.glsl"
}

int main()
{
    testSkyIllumination();
    const float horizon=std::sin(glm::radians(AppConfig::Terrain::kSolarHorizonFadeDegrees));
    float previousSolar=0;
    for(int i=0;i<=20000;++i)
    {
        float y=-1.0f+float(i)/10000.0f;
        float solar=Shader::terrainSolarVisibility(y,horizon);
        assert(solar>=previousSolar && solar<=1);
        assert(solar-previousSolar<0.01f);
        if(y<=0) assert(solar==0);
        if(y>=horizon) assert(solar==1);
        previousSolar=solar;
    }
    const Shader::CloudDensityField field{{2,2,0,0.7f},{-10,-10,1,21}};
    for(int samples:{1,8,32})
    {
        glm::vec4 params(0.3f,0,1,float(samples));
        const auto transmission=[&](glm::vec3 origin,glm::vec3 ray,glm::vec4 p) {
            return Shader::terrainCloudTransmission(origin,ray,field,p,0,0);
        };
        assert(std::abs(transmission({0,0,0},{0,1,0},params)-std::exp(-0.7f*0.3f*2))<1e-6f);
        assert(std::abs(transmission({0,3,0},{0,1,0},params)-std::exp(-0.7f*0.3f))<1e-6f);
        assert(transmission({0,5,0},{0,1,0},params)==1);
        assert(transmission({20,0,0},{0,1,0},params)==1);
        assert(std::abs(transmission({0,3,0},{1,1e-8f,0},params)-std::exp(-0.7f*0.3f*10))<1e-6f);
        auto disabled=params; disabled.z=0;
        assert(transmission({0,0,0},{0,1,0},disabled)==1);
        auto zero=params; zero.x=0;
        assert(transmission({0,0,0},{0,1,0},zero)==1);
        float previous=1;
        for(int i=0;i<=100;++i)
        {
            params.x=float(i)*0.05f;
            float t=transmission({0,0,0},{0,1,0},params);
            assert(std::isfinite(t) && t>=0 && t<=previous);
            previous=t;
        }
        params={5,0.01f,1,float(samples)};
        float terminated=transmission({0,0,0},{0,1,0},params);
        assert(terminated>0 && terminated<=params.y);
    }
    const auto circularError=[](double a,double b) { return std::abs(std::remainder(a-b,1.0)); };
    for(float angle:{0.0f,0.7f,-2.1f}) for(float stretch:{1.0f,4.0f,16.0f})
    {
        const float frequency=0.005f*0.001f;
        const auto transform=cloudMacroTransform(frequency,angle,stretch);
        const glm::dmat2 precise(transform);
        const glm::dvec2 d(std::cos(angle),std::sin(angle)), t(-d.y,d.x);
        assert(std::abs(glm::length(precise*t)-double(frequency)/stretch)<1e-12);
        for(auto cell:{0LL,-123LL,1LL<<60,-(1LL<<60)})
        {
            Position origin{cell,-cell,{524280,2000,8}};
            for(glm::dvec3 shift:{glm::dvec3(32,500,-32),glm::dvec3(-524320,-500,524320)})
            {
                auto moved=origin.translated(shift);
                auto p=WorldPhase::periodicWorldPhase(origin,precise);
                auto q=WorldPhase::periodicWorldPhase(moved,precise);
                auto delta=precise*glm::dvec2(shift.x,shift.z);
                for(int axis=0;axis<2;++axis) assert(circularError(p[axis]+delta[axis],q[axis])<1e-9);
                // Same shader-relative fixed point and same animation under either origin.
                glm::vec2 relative(100,-50), displaced=relative-glm::vec2(shift.x,shift.z);
                auto uv=transform*relative+glm::vec2(p);
                auto other=transform*displaced+glm::vec2(q);
                for(int axis=0;axis<2;++axis) assert(circularError(uv[axis],other[axis])<1e-6);
            }
        }
        double travel=0.9999,evolution=0.9999;
        const double initial=travel;
        advanceCloudMacroPhases(travel,evolution,100,frequency,1,10);
        auto movement=precise*(d*100.0*10.0);
        assert(circularError(movement.x-travel,-initial)<1e-9); // Visible motion is +d.
        assert(std::abs(movement.y)<1e-9);
        assert(travel>=0 && travel<1 && evolution>=0 && evolution<1);
        const auto frozenTravel=travel,frozenEvolution=evolution;
        advanceCloudMacroPhases(travel,evolution,0,frequency,0,10000);
        assert(travel==frozenTravel && evolution==frozenEvolution);
        double subdividedTravel=travel,subdividedEvolution=evolution;
        advanceCloudMacroPhases(travel,evolution,5,frequency,0.1f,3600);
        for(int i=0;i<36000;++i)
            advanceCloudMacroPhases(subdividedTravel,subdividedEvolution,5,frequency,0.1f,0.1);
        assert(circularError(travel,subdividedTravel)<1e-10);
        assert(circularError(evolution,subdividedEvolution)<1e-10);
    }
    for(float start:{0.0f,0.65f,0.95f}) for(float strength:{0.0f,2.0f,16.0f})
    {
        float previous=1;
        for(int i=0;i<=1000;++i)
        {
            float h=float(i)/1000, top=Shader::cloudTopFalloff(h,start,strength);
            assert(std::isfinite(top) && top>=0 && top<=previous);
            if(h<=start) assert(top==1);
            if(i==1000) assert(top==0);
            if(start==0.65f && strength==0)
                assert(std::abs(top-(1-glm::smoothstep(0.65f,1.0f,h)))<2e-7f);
            previous=top;
        }
    }
    for(float macro:{0.0f,0.3f,1.0f}) for(float detail:{0.0f,0.4f,1.0f})
        for(float offset:{0.0f,0.15f,0.5f,1.5f})
        {
            assert(Shader::cloudMacroCoverage(macro,detail,0,offset)==glm::clamp(macro+offset-0.5f,0.0f,1.0f));
            for(float strength:{0.0f,0.5f,1.0f})
            {
                float coverage=Shader::cloudMacroCoverage(macro,detail,strength,offset);
                assert(coverage>=0 && coverage<=1);
                assert(coverage<=Shader::cloudMacroCoverage(macro,detail,0,offset));
            }
            assert(std::abs(Shader::cloudMacroCoverage(macro,detail,1,offset)-glm::clamp(macro*detail+offset-0.5f,0.0f,1.0f))<1e-7f);
        }
    using glm::vec3; using glm::vec2;
    const vec3 low(-10,2,-10), high(10,4,10);
    assert(Shader::cloudDomainInterval(vec3(0,3,0),vec3(1,0,0),low,high,100)==vec2(0,10));
    assert(Shader::cloudDomainInterval(vec3(-20,3,0),vec3(1,0,0),low,high,100)==vec2(10,30));
    auto away=Shader::cloudDomainInterval(vec3(-20,3,0),vec3(-1,0,0),low,high,100);
    assert(away.y<=away.x);
    auto outside=Shader::cloudDomainInterval(vec3(0,5,0),vec3(1,0,0),low,high,100);
    assert(outside.y<=outside.x);
    assert(Shader::cloudDomainInterval(vec3(0,0,0),vec3(0,1,0),low,high,100)==vec2(2,4));
    assert(Shader::cloudDomainInterval(vec3(0,5,0),vec3(0,-1,0),low,high,100)==vec2(1,3));
    assert(Shader::cloudDomainInterval(vec3(-20,3,0),vec3(1,0,0),low,high,15)==vec2(10,15));
    vec3 shift(100000,2000,-300000);
    assert(Shader::cloudDomainInterval(vec3(-20,3,0)+shift,vec3(1,0,0),low+shift,high+shift,100)==vec2(10,30));
    assert(Shader::cloudStepBoundary(0,3000000)==0);
    assert(Shader::cloudStepBoundary(1,3000000)==3000000);
    assert(Shader::cloudStepBoundary(0.1f,3000000)<300000);
    assert(waterCloudSampleCount(48,0.5f)==24);
    assert(waterCloudSampleCount(6,0.5f)==3);
    assert(waterCloudSampleCount(48,1.0f/3.0f)==16);
    assert(waterCloudSampleCount(6,1.0f/3.0f)==2);
    for(int primary=1;primary<=128;++primary)
        for(float multiplier:{-1.0f,0.1f,1.0f/3.0f,0.5f,1.0f,2.0f})
        {
            int count=waterCloudSampleCount(primary,multiplier);
            assert(count>=1 && count<=primary);
        }
    using Id=CloudManager::TileId;
    for(auto cell:{Id{0,0},Id{-125,721},Id{(1LL<<60),-(1LL<<60)}})
        for(int i=0;i<17;++i)
        {
            assert(CloudManager::sample(173,cell,16,i)==CloudManager::sample(173,{cell.x+1,cell.y},0,i));
            assert(CloudManager::sample(173,cell,i,16)==CloudManager::sample(173,{cell.x,cell.y+1},i,0));
        }
    CloudManager manager;
    std::array<float,128*81> upload{};
    for(int iteration=0;iteration<150;++iteration)
    {
        Id center{(1LL<<60)+iteration*3,-(1LL<<60)-iteration};
        assert(manager.update(center,173));
        assert(!manager.update(center,173));
        manager.writeTexture(upload,128);
        for(int y=0;y<81;++y) for(int x=0;x<81;++x)
        {
            int tx=std::min(x/16,4),ty=std::min(y/16,4);
            assert(upload[y*128+x]==CloudManager::sample(173,{center.x+tx-2,center.y+ty-2},x-tx*16,y-ty*16));
        }
    }
    auto before=upload;
    manager.update(manager.center(),174); manager.writeTexture(upload,128);
    assert(upload!=before);
    // A fixed world point sampled from either side of a render-origin cell crossing.
    Position a{1LL<<60,-(1LL<<60),{524280,2000,55}}, b=a.translated({32,0,0});
    for(double cycles:{double(0.4f*0.001f),double(1.8f*0.001f)})
    {
        auto pa=WorldPhase::periodicWorldPhase(a,cycles),pb=WorldPhase::periodicWorldPhase(b,cycles);
        double error=WorldPhase::fract(pa.x+32*cycles)-pb.x;
        assert(std::abs(error)<1e-9);
    }
    using namespace Shader;
    // A displaced surface and slab translated together preserve the ray interval.
    for(float shift:{-524288.0f,0.0f,524288.0f})
    {
        float surface=25.0f-shift, base=1800.0f-shift;
        assert(cloudSlabInterval(surface-base,1,6500,180000)==vec2(1775,8275));
        assert(cloudSlabInterval(surface-base,1,6500,2000)==vec2(1775,2000));
        auto miss=cloudSlabInterval(surface-base,-1,6500,180000);
        assert(miss.y<=miss.x);
    }
    assert(cloudSlabInterval(-100,1,200,1000)==vec2(100,300));
    assert(cloudSlabInterval(300,-1,200,1000)==vec2(100,300));
    assert(cloudSlabInterval(100,0,200,1000)==vec2(0,1000));
    assert(cloudSlabInterval(-100,0,200,1000)==vec2(0));
    assert(cloudSlabInterval(100,1,200,50)==vec2(0,50));
    assert(cloudSlabInterval(100,-1,200,1000)==vec2(0,100));
    for(int i=0;i<12;++i)
    {
        vec3 p=vec3(float(i+1)/32.0f,float(i+3)/32.0f,float(i+2)/64.0f);
        vec2 n=cloudNoise(p);
        assert(n.x>=0 && n.x<=1 && n.y>=0 && n.y<=1);
        for(int axis=0;axis<3;++axis)
        {
            vec3 shifted=p; shifted[axis]+=1.0f;
            assert(glm::length(cloudNoise(shifted)-n)<1e-6f);
            shifted=p; shifted[axis]-=1.0f;
            assert(glm::length(cloudNoise(shifted)-n)<1e-6f);
        }
    }
    assert(std::abs(cloudHG(0,0)-1.0/(4*3.141592653589793))<1e-7);
    assert(cloudDualHG(1,0.6f,0.8f)>cloudDualHG(-1,0.6f,0.8f));
    for(float tau:{0.0f,0.1f,1.0f,10.0f,100.0f})
    {
        float single=cloudMultipleScattering(tau,0.3f,0.65f,0.8f,1,vec3(0.5f));
        assert(std::abs(single-cloudDualHG(0.3f,0.65f,0.8f)*std::exp(-tau))<1e-6);
        assert(cloudMultipleScattering(tau,0.3f,0.65f,0.8f,8,vec3(0.5f))>=single);
        assert(cloudPowder(tau,1,1,1)==1);
        assert(std::abs(cloudPowder(tau,-1,1,1)-2*(1-std::exp(-2*tau)))<1e-6);
    }
    // Analytic per-step transmission is invariant to subdivisions in a uniform medium.
    for(int steps:{1,8,48,128})
    {
        double t=1,r=0,step=std::exp(-0.003*0.4*2500/steps);
        for(int i=0;i<steps;++i) { r+=t*2.0*(1-step); t*=step; }
        assert(std::abs(t-std::exp(-3.0))<1e-12);
        assert(std::abs(r-2*(1-t))<1e-12);
    }
    std::cout<<"Cloud borders, active coverage, seed invalidation, universe phases and scattering passed\n";
}
