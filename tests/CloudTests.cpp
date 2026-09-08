#ifdef NDEBUG
#undef NDEBUG
#endif
#include "CloudManager.hpp"
#include "CloudSampling.hpp"
#include "PeriodicWorldPhase.hpp"
#include <glm/glm.hpp>
#include <cassert>
#include <iostream>
#include <limits>

namespace Shader
{
using namespace glm;
// Float literals in GLSL are floats; use matching overloads for the CPU compilation.
inline float max(float a,double b) { return glm::max(a,float(b)); }
inline float clamp(float a,double b,double c) { return glm::clamp(a,float(b),float(c)); }
inline float mix(double a,float b,float c) { return glm::mix(float(a),b,c); }
#include "../shaders/cloud_optics.glsl"
#include "../shaders/cloud_noise.glsl"
#include "../shaders/cloud_slab.glsl"
}

int main()
{
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
