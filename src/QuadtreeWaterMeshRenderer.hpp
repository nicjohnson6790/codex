#pragma once

#include "EngineRendererBase.hpp"
#include "LightingSystem.hpp"
#include "Position.hpp"
#include "WaterSettings.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <filesystem>

class QuadtreeWaterMeshRenderer : private EngineRendererBase
{
public:
    struct WaterUniforms
    {
        glm::mat4 viewProjection{ 1.0f };
        glm::vec4 cameraAndTime{ 0.0f };
        glm::vec4 waterParams{ 0.0f };
        glm::vec4 sunDirectionIntensity{ 0.0f, 1.0f, 0.0f, 1.0f };
        glm::vec4 sunColorAmbient{ 1.0f, 1.0f, 1.0f, 0.2f };
        glm::vec4 debugParams{ 0.0f };
        glm::vec4 cascadeWorldSizesA{ 0.0f };
        glm::vec4 cascadeWorldSizesB{ 0.0f };
    };

    QuadtreeWaterMeshRenderer() = default;
    ~QuadtreeWaterMeshRenderer() = default;

    QuadtreeWaterMeshRenderer(const QuadtreeWaterMeshRenderer&) = delete;
    QuadtreeWaterMeshRenderer& operator=(const QuadtreeWaterMeshRenderer&) = delete;

    void initialize(
        SDL_GPUDevice* device,
        SDL_GPUTextureFormat colorFormat,
        SDL_GPUTextureFormat depthFormat,
        const std::filesystem::path& shaderDirectory);
    void shutdown();

    void clear();
    void setActiveCamera(const Position& cameraPosition);
    void setSettings(const WaterSettings& settings);
    void rebuildMesh();

    void addLeaf(
        const WorldGridQuadtreeLeafId& leafId,
        const Position& leafOrigin,
        double leafSizeMeters,
        std::uint8_t quadtreeLodHint,
        std::uint32_t bandMask);

    void upload(SDL_GPUCopyPass* copyPass);
    void dispatchWaterSimulation(
        SDL_GPUCommandBuffer* commandBuffer,
        float timeSeconds,
        std::uint64_t frameIndex);
    void render(
        SDL_GPURenderPass* renderPass,
        SDL_GPUCommandBuffer* commandBuffer,
        const glm::mat4& viewProjection,
        const LightingSystem& lightingSystem,
        float timeSeconds) const;

    [[nodiscard]] std::uint32_t instanceCount() const;
    [[nodiscard]] std::uint32_t totalInstanceCount() const;

private:
    struct Vertex
    {
        float localCoord[2]{};
    };

    struct alignas(16) InstanceData
    {
        float position[3]{};
        std::uint32_t packedMetadata = 0;
        glm::vec4 leafParams{ 0.0f };
    };

    struct MeshResources
    {
        SDL_GPUBuffer* vertexBuffer = nullptr;
        SDL_GPUTransferBuffer* vertexTransferBuffer = nullptr;
        SDL_GPUBuffer* indexBuffer = nullptr;
        SDL_GPUTransferBuffer* indexTransferBuffer = nullptr;
        std::uint32_t vertexResolution = 0;
        std::uint32_t indexCount = 0;
    };

    struct InstanceResources
    {
        SDL_GPUBuffer* instanceBuffer = nullptr;
        SDL_GPUTransferBuffer* instanceTransferBuffer = nullptr;
        std::array<InstanceData, AppConfig::Water::kMaxWaterInstances> instances{};
        std::uint32_t instanceCount = 0;
    };

    struct WaterSimulationUniforms
    {
        glm::uvec4 dispatchParams{ 0u };
        glm::vec4 timeAndGlobal{ 0.0f };
        glm::vec4 cascadeWorldSizesA{ 0.0f };
        glm::vec4 cascadeWorldSizesB{ 0.0f };
        glm::vec4 cascadeAmplitudesA{ 0.0f };
        glm::vec4 cascadeAmplitudesB{ 0.0f };
        glm::vec4 cascadeWindDirXA{ 0.0f };
        glm::vec4 cascadeWindDirXB{ 0.0f };
        glm::vec4 cascadeWindDirZA{ 0.0f };
        glm::vec4 cascadeWindDirZB{ 0.0f };
        glm::vec4 cascadeChoppinessA{ 0.0f };
        glm::vec4 cascadeChoppinessB{ 0.0f };
    };

    static_assert(sizeof(InstanceData) % 16 == 0);

    [[nodiscard]] static std::uint32_t packMetadata(
        std::uint8_t quadtreeLodHint,
        std::uint32_t bandMask);

    void createPipeline(const std::filesystem::path& shaderDirectory);
    void createWaterComputePipelines(const std::filesystem::path& shaderDirectory);
    void createMesh();
    void createMeshGeometry(std::uint32_t vertexResolution);
    void destroyMesh();
    void createInstanceBuffer();
    void destroyInstanceBuffer();
    void createWaterTextures();
    void destroyWaterTextures();
    void createWaterSampler();
    void destroyWaterSampler();
    [[nodiscard]] WaterUniforms buildWaterUniforms(
        const glm::mat4& viewProjection,
        const LightingSystem& lightingSystem,
        float timeSeconds) const;
    [[nodiscard]] WaterSimulationUniforms buildSimulationUniforms(
        float timeSeconds,
        std::uint32_t cascadeIndex,
        std::uint32_t stageIndex,
        std::uint32_t stageAxis) const;
    void dispatchSpectrumUpdate(SDL_GPUCommandBuffer* commandBuffer, const WaterSimulationUniforms& uniforms);
    void dispatchFftStages(SDL_GPUCommandBuffer* commandBuffer, float timeSeconds);
    void dispatchBuildMaps(SDL_GPUCommandBuffer* commandBuffer, const WaterSimulationUniforms& uniforms);

    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;
    SDL_GPUComputePipeline* m_spectrumUpdatePipeline = nullptr;
    SDL_GPUComputePipeline* m_fftStagePipeline = nullptr;
    SDL_GPUComputePipeline* m_buildMapsPipeline = nullptr;
    MeshResources m_mesh{};
    InstanceResources m_instances{};
    SDL_GPUTexture* m_spectrumPing = nullptr;
    SDL_GPUTexture* m_spectrumPong = nullptr;
    SDL_GPUTexture* m_displacementTexture = nullptr;
    SDL_GPUTexture* m_slopeTexture = nullptr;
    SDL_GPUSampler* m_waterSampler = nullptr;
    WaterSettings m_settings{};
};
