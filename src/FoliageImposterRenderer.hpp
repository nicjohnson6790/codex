#pragma once

#include "EngineRendererBase.hpp"
#include "FoliageTypes.hpp"

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <filesystem>

class FoliageImposterRenderer : private EngineRendererBase
{
public:
    struct Vertex
    {
        float endpoint = 0.0f;
    };

    struct Uniforms
    {
        glm::mat4 viewProjection{ 1.0f };
    };

    struct alignas(16) DrawMetadataGpu
    {
        glm::vec4 pageOriginAndTerrainSize{ 0.0f };
        glm::vec4 terrainOriginAndSlice{ 0.0f };
        glm::uvec4 seedData{ 0u };
    };

    FoliageImposterRenderer() = default;
    ~FoliageImposterRenderer() = default;

    FoliageImposterRenderer(const FoliageImposterRenderer&) = delete;
    FoliageImposterRenderer& operator=(const FoliageImposterRenderer&) = delete;

    void initialize(
        SDL_GPUDevice* device,
        SDL_GPUTextureFormat colorFormat,
        SDL_GPUTextureFormat depthFormat,
        const std::filesystem::path& shaderDirectory);
    void shutdown();

    void clear();
    void setActiveCamera(const Position& cameraPosition);

    void addPageDraw(const FoliagePageDrawReference& drawReference);
    void upload(SDL_GPUCopyPass* copyPass);
    void render(
        SDL_GPURenderPass* renderPass,
        SDL_GPUCommandBuffer* commandBuffer,
        const glm::mat4& viewProjection,
        SDL_GPUBuffer* terrainHeightmapBuffer) const;

    [[nodiscard]] std::uint32_t drawCount() const { return m_drawCount; }
    [[nodiscard]] std::uint32_t emittedInstanceCount() const { return m_emittedInstanceCount; }
    [[nodiscard]] SDL_GPUBuffer* pagePoolBuffer() const { return m_pagePoolBuffer; }

private:
    [[nodiscard]] static SDL_GPUIndirectDrawCommand makeDrawCommand(
        std::uint32_t vertexCount,
        std::uint32_t instanceCount,
        std::uint32_t firstVertex,
        std::uint32_t firstInstance);
    void createPipeline(const std::filesystem::path& shaderDirectory);
    void createPagePoolBuffers();
    void createIndirectAndDrawMetadataBuffers();
    void createMarkerVertexBuffer();

    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;
    SDL_GPUBuffer* m_markerVertexBuffer = nullptr;
    SDL_GPUTransferBuffer* m_markerVertexTransferBuffer = nullptr;
    SDL_GPUBuffer* m_pagePoolBuffer = nullptr;
    SDL_GPUBuffer* m_drawMetadataBuffer = nullptr;
    SDL_GPUTransferBuffer* m_drawMetadataTransferBuffer = nullptr;
    SDL_GPUBuffer* m_indirectBuffer = nullptr;
    SDL_GPUTransferBuffer* m_indirectTransferBuffer = nullptr;

    std::array<DrawMetadataGpu, AppConfig::Foliage::kMarkerPageDrawCapacity> m_drawMetadata{};
    std::array<SDL_GPUIndirectDrawCommand, AppConfig::Foliage::kMarkerPageDrawCapacity> m_drawCommands{};
    std::array<std::uint32_t, FoliageConfig::kPagePoolCapacity> m_pageDrawSlots{};
    std::array<std::uint8_t, AppConfig::Foliage::kMarkerPageDrawCapacity> m_drawTerrainScalePows{};
    std::uint32_t m_drawCount = 0;
    std::uint32_t m_emittedInstanceCount = 0;
};
