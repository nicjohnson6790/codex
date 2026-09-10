#pragma once
#include "SkyIlluminationManager.hpp"
#include <memory>

namespace SkyShader
{
using namespace glm;
#include "../shaders/sky_illumination_math.glsl"
}
inline void testSkyIllumination()
{
    for(int probe=0;probe<16;++probe)
    {
        const auto fraction=SkyShader::skyProbeFraction(probe);
        assert(fraction.x==0.125f+float(probe%4)*0.25f);
        assert(fraction.y==0.125f+float(probe/4)*0.25f);
        assert(SkyShader::skyProbeGridCoordinate(fraction)==glm::vec2(probe%4,probe/4));
    }
    assert(SkyShader::skyProbeGridCoordinate(glm::vec2(0))==glm::vec2(0));
    assert(SkyShader::skyProbeGridCoordinate(glm::vec2(1))==glm::vec2(3));
    // Actual hemisphere quadrature / SH convolution: constant upper sky gives
    // pi radiance upward, pi/2 sideways, and no downward incident irradiance.
    std::array<float,9> coefficients{};
    for(int i=0;i<256;++i)
    {
        auto d=SkyShader::skyProbeDirection(i);
        assert(d.y>0 && std::abs(glm::length(d)-1.0f)<1e-6f);
        for(int c=0;c<9;++c)
            coefficients[c]+=SkyShader::skyProbeBasis(c,d)*6.28318530718f/256*SkyShader::skyProbeConvolution(c);
    }
    for(auto n:{glm::vec3(0,1,0),glm::vec3(0,-1,0),glm::vec3(1,0,0),glm::normalize(glm::vec3(1,1,0))})
    {
        float result=0;
        for(int c=0;c<9;++c) result+=coefficients[c]*SkyShader::skyProbeBasis(c,n);
        assert(std::abs(result-3.14159265359f*(1+n.y)*0.5f)<0.01f);
    }
    using Manager=SkyIlluminationManager;
    auto manager=std::make_unique<Manager>();
    std::array<Manager::Tile,16> tiles{};
    unsigned index=0;
    for(unsigned a=0;a<4;++a) for(unsigned b=0;b<4;++b)
    {
        auto path=WorldGridQuadtreeLeafId::appendChild(0,a);
        tiles[index++]={{0,0,WorldGridQuadtreeLeafId::appendChild(path,b)},0,1};
    }
    Position origin{0,0,{100,3000,200}};
    std::array<bool,1024> seen{};
    for(int frame=0;frame<4;++frame)
    {
        manager->prepare(tiles,origin,4,frame/60.0);
        assert(manager->jobs().size()==4);
        for(const auto& job:manager->jobs())
        {
            assert(job.indices.z==1 && !seen[job.indices.y]);
            assert(job.region.w==-3000);
            seen[job.indices.y]=true;
        }
    }
    manager->prepare(tiles,origin,4,1);
    for(const auto& job:manager->jobs()) assert(job.indices.z==0 && job.timing.x>0 && job.timing.x<=1);
    // Candidate lists evaluated from either side of every adjacent edge must
    // contain precisely the same nonzero contributors, also at corners.
    auto contributors=[&](unsigned region,glm::vec2 p)
    {
        std::array<float,Manager::kVisibleCapacity> result{};
        const auto& receiver=manager->regions()[region];
        for(unsigned k=0;k<receiver.lookup.z;++k)
        {
            auto candidate=manager->candidates()[receiver.lookup.y+k];
            result[candidate]=SkyShader::skyProbeRegionWeight(p,manager->regions()[candidate].domain);
        }
        return result;
    };
    for(unsigned a=0;a<16;++a) for(unsigned b=a+1;b<16;++b)
    {
        auto ra=manager->regions()[a].domain,rb=manager->regions()[b].domain;
        glm::vec2 low=glm::max(glm::vec2(ra),glm::vec2(rb));
        glm::vec2 high=glm::min(glm::vec2(ra)+ra.z,glm::vec2(rb)+rb.z);
        if(low.x<=high.x && low.y<=high.y)
            assert(contributors(a,(low+high)*0.5f)==contributors(b,(low+high)*0.5f));
    }
    // Height regeneration forces initialization, avoiding stale GPU payload blending.
    tiles[0].heightRevision=2;
    manager->prepare(tiles,origin,1,2);
    assert(manager->jobs().size()==1 && manager->jobs()[0].indices.z==1);
    auto expected=Manager::region(tiles[0].id,origin);
    assert(manager->jobs()[0].region==expected);
    // Split fallback comes from a ready parent; hidden tiles never enter the job list.
    Manager::Tile child{tiles[0]}; child.id.subdivisionPath=WorldGridQuadtreeLeafId::appendChild(child.id.subdivisionPath,0);
    std::array<Manager::Tile,2> split{tiles[1],child}; split[0].heightRevision=3;
    manager->prepare(split,origin,1,3);
    assert(manager->regions()[1].lookup.x!=kUnavailableCacheIndex);
    assert(manager->regions()[1].source.z==float(worldGridQuadtreeLeafSize(tiles[0].id)));
    // Replace one leaf by four children. Test both half-edges and corners against
    // the neighboring coarse tile, including interpolation support from a third tile.
    std::array<Manager::Tile,19> mixed{};
    std::copy(tiles.begin()+1,tiles.end(),mixed.begin());
    for(unsigned q=0;q<4;++q)
    {
        mixed[15+q]=tiles[0];
        mixed[15+q].id.subdivisionPath=WorldGridQuadtreeLeafId::appendChild(tiles[0].id.subdivisionPath,q);
    }
    manager->prepare(mixed,origin,32,4);
    for(unsigned a=0;a<mixed.size();++a) for(unsigned b=a+1;b<mixed.size();++b)
    {
        auto ra=manager->regions()[a].domain,rb=manager->regions()[b].domain;
        glm::vec2 low=glm::max(glm::vec2(ra),glm::vec2(rb));
        glm::vec2 high=glm::min(glm::vec2(ra)+ra.z,glm::vec2(rb)+rb.z);
        if(low.x<=high.x && low.y<=high.y)
            for(float t:{0.0f,0.1f,0.333333f,0.5f,0.9f,1.0f})
                assert(contributors(a,glm::mix(low,high,t))==contributors(b,glm::mix(low,high,t)));
    }
    // Churn beyond capacity; retained visible data must survive admissions and
    // reassigned slots must always initialize, never blend a previous tile's light.
    unsigned retainedRefreshes=0;
    for(int frame=0;frame<2200;++frame)
    {
        std::array<Manager::Tile,2> churn{tiles[0],Manager::Tile{{frame+10,0,0},0,1}};
        manager->prepare(churn,origin,1,5+frame/60.0);
        assert(manager->jobs().size()==1);
        if(manager->jobs()[0].indices.z==0) ++retainedRefreshes;
        assert(manager->regions()[0].lookup.x!=kUnavailableCacheIndex);
    }
    assert(retainedRefreshes>0); // New visible tiles cannot starve existing lighting.
    // Integer-origin subtraction precedes float conversion even at universe scale.
    auto shifted=tiles[0].id; shifted.gridX=1LL<<55; shifted.gridY=-(1LL<<55);
    Position huge{shifted.gridX,shifted.gridY,origin.localPosition()};
    assert(Manager::region(shifted,huge)==expected);
    manager->prepare({},origin,4,4);
    assert(manager->jobs().empty() && manager->regions().empty() && manager->fallback()==kUnavailableCacheIndex);
}
