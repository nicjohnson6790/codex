#pragma once
#include "CloudManager.hpp"
#include "SkyboxRenderer.hpp"
#include <cstddef>

class CloudRenderer : private EngineRendererBase
{
public:
    struct Settings
    {
        bool enabled=true;
        std::uint32_t seed=173;
        int noiseDimensions[3]{32,32,64}; // Applied on initialization; no CPU volume.
        float baseAltitude=1800, thickness=6500, coverage=0.15f, density=3.5f;
        float baseNoiseScale=0.05f, detailNoiseScale=0.1f; // cycles / km
        float baseStrength=0.08f, detailStrength=0.28f, erosion=0.7f;
        float windDirection=0.4f, windSpeed=15;
        float extinction=0.003f, anisotropy=0.65f, lobeWeight=0.8f;
        float powderStrength=1, powderAngularPower=1;
        int octaveCount=8, viewSteps=48, sunSteps=6;
        float octaveA=0.5f, octaveB=0.5f, octaveC=0.5f;
        float waterSamplingMultiplier=0.5f;
        float ambient=0.15f, maxDistance=180000, termination=0.01f;
    };
    struct DensityUniforms
    {
        glm::vec4 layer; // base relative to origin, thickness, coverage, density
        glm::vec4 macro; // origin-relative minimum XZ, pitch, dimension
        glm::vec4 basePhase; // XYZ bounded phase, cycles/m
        glm::vec4 detailPhase;
        glm::vec4 shape; // base strength, detail strength, erosion, unused
    };
    struct SamplingState
    {
        DensityUniforms field;
        glm::vec4 sun, atmosphere;
        SkyboxRenderer::AtmosphereOptics optics;
        glm::vec4 scattering, powder, march, octaves;
    };
    static_assert(sizeof(DensityUniforms)==80);
    struct Uniforms
    {
        glm::mat4 inverseViewProjection;
        SamplingState cloud;
    };
    struct SamplingResources
    {
        SDL_GPUTextureSamplerBinding coverage, noise;
        SamplingState state;
    };
    static_assert(sizeof(SamplingState)==256);
    static_assert(offsetof(SamplingState,optics)==112);
    static_assert(offsetof(SamplingState,scattering)==192);
    static_assert(offsetof(SamplingState,march)==224);
    static_assert(sizeof(Uniforms)==320);
    static_assert(offsetof(Uniforms,cloud)==64);
    SamplingResources waterSamplingResources() const;
    Settings& settings() { return m_settings; }
    void drawSettings();
    void initialize(SDL_GPUDevice*,SDL_GPUTextureFormat,SDL_GPUTextureFormat,const std::filesystem::path&);
    void shutdown();
    void upload(SDL_GPUCopyPass*,const Position&,double timeSeconds,const SkyboxRenderer&,const LightingSystem&);
    void render(SDL_GPURenderPass*,SDL_GPUCommandBuffer*,const glm::mat4&,SDL_GPUTexture*);
    CloudManager& manager() { return m_manager; }
private:
    Settings m_settings;
    CloudManager m_manager;
    SDL_GPUTexture *m_macro=nullptr,*m_noise=nullptr;
    SDL_GPUTransferBuffer* m_upload=nullptr;
    SDL_GPUSampler *m_linear=nullptr,*m_repeat=nullptr,*m_depth=nullptr;
    SDL_GPUGraphicsPipeline* m_pipeline=nullptr;
    DensityUniforms m_field{};
    SamplingState m_prepared{};
    glm::vec2 m_waterSteps{1.0f};
    std::uint64_t m_uploadedRevision=0;
    glm::dvec2 m_baseWind{},m_detailWind{};
    double m_previousTime=0;
};
