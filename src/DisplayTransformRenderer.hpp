#pragma once
#include "EngineRendererBase.hpp"

class DisplayTransformRenderer : private EngineRendererBase
{
public:
    void initialize(SDL_GPUDevice* device, SDL_GPUTextureFormat sourceFormat,
        SDL_GPUTextureFormat displayFormat, const std::filesystem::path& shaders);
    void shutdown();
    void render(SDL_GPURenderPass* pass, SDL_GPUCommandBuffer* command, SDL_GPUTexture* scene);
    float exposure = 1.0f;
    bool automatic = true;
    float compensationEV = 0, minimumEV = -12, maximumEV = 16;
    float brightenSceneSeconds = 0.5f, darkenSceneSeconds = 2.0f;
    float deltaSeconds = 1.0f / 60.0f;
    void meter(SDL_GPUCommandBuffer*, SDL_GPUTexture*, unsigned width, unsigned height);
    void drawSettings();
private:
    SDL_GPUComputePipeline* m_compute[3]{};
    SDL_GPUBuffer* m_histogram = nullptr;
    SDL_GPUBuffer* m_state = nullptr;
    bool m_initialized = false, m_wasAutomatic = true;
    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;
    SDL_GPUSampler* m_sampler = nullptr;
};
