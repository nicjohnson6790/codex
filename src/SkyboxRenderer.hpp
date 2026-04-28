#pragma once

#include "EngineRendererBase.hpp"
#include "LightingSystem.hpp"

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>

#include <filesystem>

class SkyboxRenderer : private EngineRendererBase
{
public:
    struct Vertex
    {
        float position[2];
    };

    struct FragmentUniforms
    {
        glm::mat4 inverseViewProjection{1.0f};
        glm::mat4 skyRotation{1.0f};
    };

    void initialize(
        SDL_GPUDevice* device,
        SDL_GPUTextureFormat colorFormat,
        SDL_GPUTextureFormat depthFormat,
        const std::filesystem::path& shaderDirectory,
        const std::filesystem::path& resourceDirectory
    );
    void shutdown();

    void render(
        SDL_GPURenderPass* renderPass,
        SDL_GPUCommandBuffer* commandBuffer,
        const glm::mat4& inverseViewProjection,
        const LightingSystem& lightingSystem) const;

private:
    void createPipeline(const std::filesystem::path& shaderDirectory);
    void createStaticVertexResources();
    void createCubemapTexture(const std::filesystem::path& resourceDirectory);

    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;
    SDL_GPUBuffer* m_vertexBuffer = nullptr;
    SDL_GPUTransferBuffer* m_vertexTransferBuffer = nullptr;
    SDL_GPUTexture* m_cubemapTexture = nullptr;
    SDL_GPUSampler* m_sampler = nullptr;
};
