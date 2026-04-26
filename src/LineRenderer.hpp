#pragma once

#include "EngineRendererBase.hpp"
#include "Position.hpp"

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <filesystem>
#include <vector>

class LineRenderer : private EngineRendererBase
{
public:
    struct Vertex
    {
        float position[3];
        float color[3];
    };

    LineRenderer() = default;
    ~LineRenderer() = default;

    LineRenderer(const LineRenderer&) = delete;
    LineRenderer& operator=(const LineRenderer&) = delete;

    void initialize(
        SDL_GPUDevice* device,
        SDL_GPUTextureFormat colorFormat,
        SDL_GPUTextureFormat depthFormat,
        const std::filesystem::path& shaderDirectory
    );
    void shutdown();

    void clear();
    void setActiveCamera(const Position& cameraPosition);
    void addLine(const Position& start, const Position& stop, const glm::vec3& color);
    void addLine(const Position& start, const Position& stop, const glm::vec3& startColor, const glm::vec3& stopColor);
    void upload(SDL_GPUCopyPass* copyPass);
    void render(SDL_GPURenderPass* renderPass, SDL_GPUCommandBuffer* commandBuffer, const glm::mat4& viewProjection) const;

private:
    void createPipeline(const std::filesystem::path& shaderDirectory);
    void ensureVertexCapacity(std::uint32_t requiredVertexCount);

    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;
    SDL_GPUBuffer* m_vertexBuffer = nullptr;
    SDL_GPUTransferBuffer* m_vertexTransferBuffer = nullptr;

    std::uint32_t m_vertexCapacity = 0;
    std::vector<Vertex> m_vertices;
};
