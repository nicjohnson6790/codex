#pragma once

#include "EngineRendererBase.hpp"
#include "Position.hpp"

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>

#include <filesystem>
#include <vector>

class TriangleRenderer : private EngineRendererBase
{
public:
    struct Vertex
    {
        float position[3];
        float color[3];
    };

    struct InstanceData
    {
        float offset[3];
    };

    TriangleRenderer() = default;
    ~TriangleRenderer() = default;

    TriangleRenderer(const TriangleRenderer&) = delete;
    TriangleRenderer& operator=(const TriangleRenderer&) = delete;

    void initialize(
        SDL_GPUDevice* device,
        SDL_GPUTextureFormat colorFormat,
        SDL_GPUTextureFormat depthFormat,
        const std::filesystem::path& shaderDirectory
    );
    void shutdown();

    void clear();
    void setActiveCamera(const Position& cameraPosition);
    void addTriangle(const Position& position);
    void upload(SDL_GPUCopyPass* copyPass);
    void render(SDL_GPURenderPass* renderPass, SDL_GPUCommandBuffer* commandBuffer, const glm::mat4& viewProjection) const;

private:
    void createPipeline(const std::filesystem::path& shaderDirectory);
    void createStaticVertexResources();
    void ensureInstanceCapacity(std::uint32_t requiredInstanceCount);

    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;

    SDL_GPUBuffer* m_vertexBuffer = nullptr;
    SDL_GPUTransferBuffer* m_vertexTransferBuffer = nullptr;
    SDL_GPUBuffer* m_instanceBuffer = nullptr;
    SDL_GPUTransferBuffer* m_instanceTransferBuffer = nullptr;

    std::uint32_t m_instanceCapacity = 0;
    std::vector<InstanceData> m_instances;
};
