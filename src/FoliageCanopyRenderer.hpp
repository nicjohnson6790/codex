#pragma once

#include "EngineRendererBase.hpp"
#include "FoliageTypes.hpp"
#include "LightingSystem.hpp"

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <filesystem>

class FoliageCanopyRenderer : private EngineRendererBase
{
public:
    struct Vertex
    {
        float uv[2]{};
        float shellY = 0.0f;
    };

    struct Uniforms
    {
        glm::mat4 viewProjection{ 1.0f };
        glm::vec4 sunDirectionIntensity{ 0.0f, 1.0f, 0.0f, 1.0f };
        glm::vec4 canopyShellParams{ 4.0f, 0.0f, 0.0f, 0.0f };
    };

    struct alignas(16) DrawMetadataGpu
    {
        glm::vec4 patchOriginAndSize{ 0.0f };
        glm::vec4 terrainOriginAndSize{ 0.0f };
        glm::vec4 terrainSliceData{ 0.0f };
        glm::uvec4 patchSeedData{ 0u };
        glm::uvec4 cellSlots[FoliageConfig::kCanopyCellCountPerNode / 4u]{};
        glm::uvec4 cellSeeds[FoliageConfig::kCanopyCellCountPerNode / 4u]{};
    };

    struct alignas(16) CellGenerationParams
    {
        glm::uvec4 dispatchParams{ 0u };
        glm::vec4 terrainParams{ 0.0f };
        glm::vec4 worldParams{ 0.0f };
    };

    FoliageCanopyRenderer() = default;
    ~FoliageCanopyRenderer() = default;

    FoliageCanopyRenderer(const FoliageCanopyRenderer&) = delete;
    FoliageCanopyRenderer& operator=(const FoliageCanopyRenderer&) = delete;

    void initialize(
        SDL_GPUDevice* device,
        SDL_GPUTextureFormat colorFormat,
        SDL_GPUTextureFormat depthFormat,
        const std::filesystem::path& shaderDirectory);
    void shutdown();

    void clear();
    void setActiveCamera(const Position& cameraPosition);

    [[nodiscard]] bool queueCellGeneration(
        const WorldGridQuadtreeLeafId& leafId,
        const WorldGridQuadtreeLeafId& terrainLeafId,
        std::uint16_t terrainSliceIndex,
        std::uint16_t canopySlotIndex,
        float waterLevel);
    void addCanopyDraw(const FoliageCanopyDrawReference& drawReference);
    void upload(SDL_GPUCopyPass* copyPass);
    void dispatchCellGenerations(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUBuffer* terrainHeightmapBuffer);
    void render(
        SDL_GPURenderPass* renderPass,
        SDL_GPUCommandBuffer* commandBuffer,
        const glm::mat4& viewProjection,
        const LightingSystem& lightingSystem,
        SDL_GPUBuffer* terrainHeightmapBuffer) const;

    [[nodiscard]] std::uint32_t drawCount() const { return m_drawCount; }

private:
    [[nodiscard]] static SDL_GPUIndexedIndirectDrawCommand makeDrawCommand(
        std::uint32_t indexCount,
        std::uint32_t instanceCount,
        std::uint32_t firstIndex,
        std::int32_t vertexOffset,
        std::uint32_t firstInstance);
    [[nodiscard]] static std::uint64_t hashLeafId(const WorldGridQuadtreeLeafId& leafId);
    [[nodiscard]] static std::uint64_t mix64(std::uint64_t x);
    void createPipeline(const std::filesystem::path& shaderDirectory);
    void createComputePipeline(const std::filesystem::path& shaderDirectory);
    void createCanopyBuffers();
    void createDrawBuffers();
    void createMeshResources();

    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;
    SDL_GPUComputePipeline* m_computePipeline = nullptr;
    SDL_GPUBuffer* m_vertexBuffer = nullptr;
    SDL_GPUTransferBuffer* m_vertexTransferBuffer = nullptr;
    SDL_GPUBuffer* m_indexBuffer = nullptr;
    SDL_GPUTransferBuffer* m_indexTransferBuffer = nullptr;
    SDL_GPUBuffer* m_cellBitsetPoolBuffer = nullptr;
    SDL_GPUBuffer* m_generationBuffer = nullptr;
    SDL_GPUTransferBuffer* m_generationTransferBuffer = nullptr;
    SDL_GPUBuffer* m_drawMetadataBuffer = nullptr;
    SDL_GPUTransferBuffer* m_drawMetadataTransferBuffer = nullptr;
    SDL_GPUBuffer* m_indirectBuffer = nullptr;
    SDL_GPUTransferBuffer* m_indirectTransferBuffer = nullptr;

    std::array<DrawMetadataGpu, AppConfig::Foliage::kCanopyDrawCapacity> m_drawMetadata{};
    std::array<CellGenerationParams, FoliageConfig::kCanopyGenerationBudgetPerFrame> m_pendingGenerations{};
    std::uint32_t m_drawCount = 0;
    std::uint32_t m_pendingGenerationCount = 0;
    std::uint32_t m_indexCount = 0;
};
