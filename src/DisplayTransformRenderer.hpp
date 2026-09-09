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
private:
    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;
    SDL_GPUSampler* m_sampler = nullptr;
};
