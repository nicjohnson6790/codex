#pragma once
#include "AssetResidency.hpp"
#include "Position.hpp"
#include <array>
#include <span>

// Semantic coverage only; GPU resources and flattened storage belong to the renderer.
class CloudManager
{
public:
    static constexpr int kIntervals = 16, kTileSamples = 17, kTextureSamples = 81;
    static constexpr double kPitch = 32768.0;
    struct TileId
    {
        std::int64_t x{}, y{};
        friend bool operator==(const TileId&, const TileId&) = default;
    };
    static std::uint64_t mix(std::uint64_t v)
    {
        v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
        v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
        return v ^ (v >> 31);
    }
    struct Hash { std::size_t operator()(TileId id) const { return mix(std::uint64_t(id.x)) ^ mix(std::uint64_t(id.y) + 0x9e3779b97f4a7c15ULL); } };
    static float sample(std::uint32_t seed, TileId id, int x, int y)
    {
        // Unsigned arithmetic also defines the wrap at the signed coordinate limits.
        auto cx = std::uint64_t(id.x) + (x == 16);
        auto cy = std::uint64_t(id.y) + (y == 16);
        x %= 16; y %= 16;
        auto h = mix(cx ^ mix(cy) ^ mix(seed) ^ mix(std::uint64_t(y * 16 + x)));
        return float(h >> 40) / 16777215.0f;
    }
    bool update(TileId center, std::uint32_t seed)
    {
        if (m_valid && center == m_center && seed == m_seed) return false;
        if (!m_valid || seed != m_seed) m_cache.clear();
        m_center = center; m_seed = seed; m_valid = true;
        m_cache.age();
        // Protect every retained desired tile before admitting any missing tile.
        for (int y=0; y<5; ++y) for (int x=0; x<5; ++x)
            if (auto slot=m_cache.find(tileId(x,y))) m_cache.touch(*slot);
        for (int y=0; y<5; ++y) for (int x=0; x<5; ++x)
        {
            const auto id=tileId(x,y);
            auto slot=m_cache.find(id);
            if (!slot)
            {
                slot=m_cache.findAllocationCandidate();
                if (!slot) throw std::logic_error("Cloud coverage cache exhausted");
                m_cache.assign(*slot,id);
                for (int sy=0; sy<17; ++sy) for (int sx=0; sx<17; ++sx)
                    m_tiles[*slot][sy*17+sx]=sample(seed,id,sx,sy);
                m_cache.markReady(*slot);
            }
            m_active[y*5+x]=*slot;
        }
        ++m_revision;
        return true;
    }
    void writeTexture(std::span<float> mapped, std::size_t rowPitch = 81) const
    {
        if (!m_valid || rowPitch < 81 || mapped.size() < rowPitch*81) throw std::logic_error("Invalid cloud upload destination");
        for (int y=0; y<81; ++y) for (int x=0; x<81; ++x)
        {
            const int tx=std::min(x/16,4), ty=std::min(y/16,4);
            mapped[y*rowPitch+x]=m_tiles[m_active[ty*5+tx]][(y-ty*16)*17+x-tx*16];
        }
    }
    TileId center() const { return m_center; }
    std::uint64_t revision() const { return m_revision; }
private:
    TileId tileId(int x,int y) const { return {std::int64_t(std::uint64_t(m_center.x)+x-2),std::int64_t(std::uint64_t(m_center.y)+y-2)}; }
    FixedAssetCache<TileId,CacheIndex,Hash> m_cache{32,32,4};
    std::array<std::array<float,289>,32> m_tiles{};
    std::array<CacheIndex,25> m_active{};
    TileId m_center{};
    std::uint32_t m_seed{};
    std::uint64_t m_revision{};
    bool m_valid=false;
};
