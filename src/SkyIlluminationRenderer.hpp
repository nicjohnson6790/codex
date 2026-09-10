#pragma once
#include "CloudRenderer.hpp"
#include "SkyIlluminationManager.hpp"

// Owns GPU probe storage and compute only. SamplingResources is intentionally
// independent of terrain shading, for later foliage/environment consumers.
class SkyIlluminationRenderer : private EngineRendererBase
{
public:
    struct SamplingResources { SDL_GPUBuffer* coefficients; SDL_GPUBuffer* regions; };
    void initialize(SDL_GPUDevice*,const std::filesystem::path&);
    void shutdown();
    void prepare(std::span<const SkyIlluminationManager::Tile>,const Position&,double seconds);
    void upload(SDL_GPUCopyPass*);
    void dispatch(SDL_GPUCommandBuffer*,SDL_GPUBuffer* heightmaps,const CloudRenderer::SamplingResources&,
                  const SkyboxRenderer&,const LightingSystem&,int cloudBudget);
    SamplingResources samplingResources() const { return {m_coefficients,m_regions}; }
    CacheIndex regionForTile(const WorldGridQuadtreeLeafId& id) const { return m_manager.regionForTile(id); }
    std::uint64_t totalTileUpdates() const { return m_totalTileUpdates; }
    void drawSettings();
    int tilesPerFrame=4;
private:
    SkyIlluminationManager m_manager;
    SDL_GPUComputePipeline* m_pipeline=nullptr;
    SDL_GPUBuffer *m_coefficients=nullptr,*m_regions=nullptr;
    SDL_GPUTransferBuffer* m_upload=nullptr;
    std::uint64_t m_totalTileUpdates=0;
    struct Uniforms
    {
        CloudRenderer::SamplingState cloud;
        glm::mat4 skyRotation;
        std::array<SkyIlluminationManager::Job,SkyIlluminationManager::kMaxUpdates> jobs;
    };
    static_assert(sizeof(Uniforms)==1904 && offsetof(Uniforms,skyRotation)==304 && offsetof(Uniforms,jobs)==368);
};
