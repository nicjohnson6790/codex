#pragma once
#include "AssetResidency.hpp"
#include "WorldGridQuadtreeTypes.hpp"
#include "SurfacePosition.hpp"
#include <glm/glm.hpp>
#include <array>
#include <span>
#include <cmath>

// Semantic residency and a bounded, queue-ordered GPU refresh schedule. No GPU ownership.
// Inputs are exactly the successfully emitted, frustum-visible terrain leaves.
class SkyIlluminationManager
{
public:
    static constexpr unsigned kCapacity=1024, kVisibleCapacity=512, kMaxUpdates=32;
    struct Tile
    {
        WorldGridQuadtreeLeafId id;
        std::uint32_t heightSlice=0;
        std::uint64_t heightRevision=0;
    };
    struct alignas(16) Job
    {
        glm::vec4 region; // relative minimum XZ, size, relative sea-level Y
        glm::uvec4 indices; // height slice, probe cache slot, reset, reserved
        glm::vec4 timing; // blend amount, reserved
    };
    struct alignas(16) Region
    {
        glm::vec4 domain; // relative minimum XZ, size, support width
        glm::vec4 source; // probe grid minimum XZ, size, unused
        glm::uvec4 lookup; // cache slot, candidate offset/count, unused
    };
    static_assert(sizeof(Job)==48 && sizeof(Region)==48);
    static_assert(offsetof(Job,indices)==16 && offsetof(Job,timing)==32);
    static_assert(offsetof(Region,lookup)==32);
    struct Hash
    {
        std::size_t operator()(const WorldGridQuadtreeLeafId& id) const
        {
            auto mix=[](std::uint64_t x) { x=(x^(x>>30))*0xbf58476d1ce4e5b9ULL; x=(x^(x>>27))*0x94d049bb133111ebULL; return x^(x>>31); };
            return mix(std::uint64_t(id.gridX))^mix(std::uint64_t(id.gridY)+1)^mix(id.subdivisionPath+2);
        }
    };
    static WorldGridQuadtreeLeafId parent(WorldGridQuadtreeLeafId id)
    {
        const auto depth=worldGridQuadtreeLeafDepth(id);
        if(depth) id.subdivisionPath &= ~(std::uint64_t(7)<<((depth-1)*3));
        return id;
    }
    static glm::vec4 region(const WorldGridQuadtreeLeafId& id,const Position& origin)
    {
        const auto bounds=worldGridQuadtreeLeafBounds(id);
        const auto relative=surfacePositionRelativeTo(bounds.first,origin);
        return {relative.x,relative.z,worldGridQuadtreeLeafSize(id),-origin.localPosition().y};
    }
    void clear()
    {
        m_cache.clear(); m_jobCount=m_regionCount=m_candidateCount=0;
        m_fallback=kUnavailableCacheIndex; m_frame=0;
    }
    void prepare(std::span<const Tile> tiles,const Position& origin,unsigned budget,double seconds)
    {
        if(tiles.size()>kVisibleCapacity) throw std::logic_error("Sky illumination visible capacity");
        ++m_frame; m_cache.age(); m_jobCount=0; m_regionCount=unsigned(tiles.size()); m_candidateCount=0;
        // Protect every current tile and retained ancestor before any reassignment.
        for(const auto& tile:tiles)
        {
            auto id=tile.id;
            for(;;)
            {
                if(auto slot=m_cache.find(id)) m_cache.touch(*slot);
                if(!id.subdivisionPath) break;
                id=parent(id);
            }
        }
        std::array<bool,kVisibleCapacity> selected{};
        for(unsigned update=0;update<std::clamp(budget,1u,kMaxUpdates);++update)
        {
            unsigned best=kVisibleCapacity;
            std::uint64_t oldest=UINT64_MAX;
            double nearest=INFINITY;
            for(unsigned i=0;i<tiles.size();++i)
            {
                if(selected[i]) continue;
                const auto slot=m_cache.find(tiles[i].id);
                const bool valid=slot && m_cache.isReady(*slot) && m_revisions[*slot]==tiles[i].heightRevision;
                // Missing work starts one refresh cycle old. This prioritizes
                // initialization without starving retained tiles during continuous flight.
                const std::uint64_t cycle=(tiles.size()+std::clamp(budget,1u,kMaxUpdates)-1)/std::clamp(budget,1u,kMaxUpdates);
                const auto last=valid ? m_lastFrame[*slot] : (m_frame>cycle ? m_frame-cycle : 0);
                const auto r=region(tiles[i].id,origin);
                const double x=double(r.x)+r.z*0.5, z=double(r.y)+r.z*0.5;
                const double distance=x*x+z*z;
                if(best==kVisibleCapacity || last<oldest || (last==oldest && distance<nearest))
                    { best=i; oldest=last; nearest=distance; }
            }
            if(best==kVisibleCapacity) break;
            selected[best]=true;
            const auto& tile=tiles[best];
            auto slot=m_cache.find(tile.id);
            bool reset=!slot || !m_cache.isReady(*slot) || m_revisions[*slot]!=tile.heightRevision;
            if(!slot)
            {
                slot=m_cache.findAllocationCandidate();
                if(!slot) continue;
                // Job storage is reserved before transactional cache admission.
                m_cache.assign(*slot,tile.id);
            }
            const float blend=reset ? 1.0f : float(1.0-std::exp(-std::max(0.0,seconds-m_lastTime[*slot])/0.25));
            m_jobs[m_jobCount++]={region(tile.id,origin),{tile.heightSlice,*slot,reset?1u:0u,0},{blend,0,0,0}};
            m_revisions[*slot]=tile.heightRevision; m_lastFrame[*slot]=m_frame; m_lastTime[*slot]=seconds;
            m_cache.markReady(*slot);
            // Ready means available to later commands on the same GPU queue. The
            // caller must dispatch every job before exposing these frame bindings.
        }
        m_fallback=kUnavailableCacheIndex;
        double closest=INFINITY;
        for(unsigned i=0;i<tiles.size();++i)
        {
            m_visibleIds[i]=tiles[i].id;
            auto id=tiles[i].id;
            CacheIndex slot=kUnavailableCacheIndex;
            for(;;)
            {
                slot=m_cache.isResident(id);
                if(id==tiles[i].id && slot!=kUnavailableCacheIndex && m_revisions[slot]!=tiles[i].heightRevision)
                    slot=kUnavailableCacheIndex;
                if(slot!=kUnavailableCacheIndex || !id.subdivisionPath) break;
                id=parent(id);
            }
            auto domain=region(tiles[i].id,origin);
            domain.w=domain.z/4.0f;
            auto source=slot!=kUnavailableCacheIndex ? region(id,origin) : domain;
            m_regions[i]={domain,source,{slot,0,0,0}};
            const double x=double(domain.x)+domain.z*0.5,z=double(domain.y)+domain.z*0.5;
            if(slot!=kUnavailableCacheIndex && x*x+z*z<closest) { m_fallback=i; closest=x*x+z*z; }
        }
        // Conservative lists contain every support that can contribute anywhere
        // in each receiver tile. Identical positions get identical weights across LOD edges.
        for(unsigned i=0;i<tiles.size();++i)
        {
            auto& a=m_regions[i]; a.lookup.y=m_candidateCount;
            for(unsigned j=0;j<tiles.size();++j)
            {
                const auto& b=m_regions[j]; if(b.lookup.x==kUnavailableCacheIndex) continue;
                const double pad=std::max(1.0f,a.domain.z*1e-5f); // Includes bridge edge roundoff.
                if(double(b.domain.x)-b.domain.w>double(a.domain.x)+a.domain.z+pad ||
                   double(b.domain.y)-b.domain.w>double(a.domain.y)+a.domain.z+pad ||
                   double(b.domain.x)+b.domain.z+b.domain.w<double(a.domain.x)-pad ||
                   double(b.domain.y)+b.domain.z+b.domain.w<double(a.domain.y)-pad) continue;
                m_candidates[m_candidateCount++]=j;
            }
            a.lookup.z=m_candidateCount-a.lookup.y;
        }
    }
    std::span<const Job> jobs() const { return {m_jobs.data(),m_jobCount}; }
    std::span<const Region> regions() const { return {m_regions.data(),m_regionCount}; }
    std::span<const std::uint32_t> candidates() const { return {m_candidates.data(),m_candidateCount}; }
    unsigned fallback() const { return m_fallback; }
    // Read-only, frame-local receiver hint for consumers attached to a terrain leaf.
    CacheIndex regionForTile(const WorldGridQuadtreeLeafId& id) const
    {
        for(unsigned i=0;i<m_regionCount;++i) if(m_visibleIds[i]==id) return CacheIndex(i);
        return kUnavailableCacheIndex;
    }
private:
    FixedAssetCache<WorldGridQuadtreeLeafId,CacheIndex,Hash,std::equal_to<WorldGridQuadtreeLeafId>,std::uint64_t> m_cache{kCapacity,kCapacity,4};
    std::array<std::uint64_t,kCapacity> m_lastFrame{},m_revisions{};
    std::array<double,kCapacity> m_lastTime{};
    std::array<Job,kMaxUpdates> m_jobs{};
    std::array<Region,kVisibleCapacity> m_regions{};
    std::array<WorldGridQuadtreeLeafId,kVisibleCapacity> m_visibleIds{};
    std::array<std::uint32_t,kVisibleCapacity*kVisibleCapacity> m_candidates{};
    unsigned m_jobCount=0,m_regionCount=0,m_candidateCount=0,m_fallback=kUnavailableCacheIndex;
    std::uint64_t m_frame=0;
};
