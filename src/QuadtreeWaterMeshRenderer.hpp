#pragma once

#include "EngineRendererBase.hpp"
#include "LightingSystem.hpp"
#include "Position.hpp"
#include "SkyboxRenderer.hpp"
#include "WaterSettings.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

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
        glm::vec4 cascadeShallowDampingA{ 0.0f };
        glm::vec4 cascadeShallowDampingB{ 0.0f };
        glm::vec4 depthEffectParams{ 0.0f };
        glm::mat4 skyRotation{ 1.0f };
        glm::vec4 atmosphereParams{ 0.0f };
        glm::vec4 sunDirectionTimeOfDay{ 0.0f };
        glm::vec4 opticalParams{ 0.0f };
        glm::vec4 refractionParams{ 0.0f };
        glm::vec4 foamParams{ 0.0f };
        glm::vec4 foamParams2{ 0.0f };
        glm::vec4 foamColor{ 0.0f };
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
        bool hasTerrainSlice,
        std::uint16_t terrainSliceIndex,
        std::uint32_t bandMask);
    void addBridge(
        const WorldGridQuadtreeLeafId& leafId,
        const Position& leafOrigin,
        double leafSizeMeters,
        std::uint8_t quadtreeLodHint,
        bool hasTerrainSlice,
        std::uint16_t terrainSliceIndex,
        std::uint32_t bandMask,
        std::uint8_t edgeIndex);
    void addCoarseBridge(
        const WorldGridQuadtreeLeafId& leafId,
        const Position& leafOrigin,
        double leafSizeMeters,
        std::uint8_t quadtreeLodHint,
        bool hasTerrainSlice,
        std::uint16_t terrainSliceIndex,
        std::uint32_t bandMask,
        std::uint8_t edgeIndex);

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
        const SkyboxRenderer& skyboxRenderer,
        float timeSeconds,
        SDL_GPUBuffer* terrainHeightmapBuffer) const;

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

    struct MeshRange
    {
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
    };

    struct InstanceResources
    {
        SDL_GPUBuffer* instanceBuffer = nullptr;
        SDL_GPUTransferBuffer* instanceTransferBuffer = nullptr;
        std::array<InstanceData, AppConfig::Water::kMaxWaterInstances> instances{};
        std::uint32_t instanceCount = 0;
    };

    struct WorkingBufferResources
    {
        SDL_GPUBuffer* initialSpectrum = nullptr;
        SDL_GPUBuffer* displacementSpectrumPing = nullptr;
        SDL_GPUBuffer* displacementSpectrumPong = nullptr;
        SDL_GPUBuffer* slopeSpectrumPing = nullptr;
        SDL_GPUBuffer* slopeSpectrumPong = nullptr;
    };

    struct WaterSimulationUniforms
    {
        // Keep this layout in sync with every water compute shader uniform block.
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
        glm::vec4 cascadeWindSpeedsA{ 0.0f };
        glm::vec4 cascadeWindSpeedsB{ 0.0f };
        glm::vec4 cascadeFetchesA{ 0.0f };
        glm::vec4 cascadeFetchesB{ 0.0f };
        glm::vec4 cascadeSpreadBlendA{ 0.0f };
        glm::vec4 cascadeSpreadBlendB{ 0.0f };
        glm::vec4 cascadeSwellA{ 0.0f };
        glm::vec4 cascadeSwellB{ 0.0f };
        glm::vec4 cascadePeakEnhancementA{ 0.0f };
        glm::vec4 cascadePeakEnhancementB{ 0.0f };
        glm::vec4 cascadeShortWavesFadeA{ 0.0f };
        glm::vec4 cascadeShortWavesFadeB{ 0.0f };
        glm::vec4 cascadeChoppinessA{ 0.0f };
        glm::vec4 cascadeChoppinessB{ 0.0f };
        glm::vec4 foamParams{ 0.0f };
        glm::vec4 foamParams2{ 0.0f };
        glm::vec4 simulationParams{ 0.0f };
    };

    static_assert(sizeof(InstanceData) % 16 == 0);
    static_assert(sizeof(WaterUniforms) % 16 == 0);
    static_assert(sizeof(WaterSimulationUniforms) % 16 == 0);

    [[nodiscard]] static std::uint32_t packMetadata(
        std::uint8_t quadtreeLodHint,
        std::uint32_t bandMask,
        std::uint8_t edgeIndex = 0);

    void createPipelines(const std::filesystem::path& shaderDirectory);
    void createWaterComputePipelines(const std::filesystem::path& shaderDirectory);
    void createMesh();
    void createMeshGeometry(std::uint32_t vertexResolution);
    void destroyMesh();
    void createMeshResources(
        const std::vector<Vertex>& vertices,
        const std::vector<std::uint32_t>& indices,
        MeshResources& resources);
    void createInstanceBuffer();
    void destroyInstanceBuffer();
    void createWorkingBuffers();
    void destroyWorkingBuffers();
    void createWaterTextures();
    void destroyWaterTextures();
    void createWaterSampler();
    void destroyWaterSampler();
    [[nodiscard]] WaterUniforms buildWaterUniforms(
        const glm::mat4& viewProjection,
        const LightingSystem& lightingSystem,
        const SkyboxRenderer& skyboxRenderer,
        float timeSeconds) const;
    [[nodiscard]] WaterSimulationUniforms buildSimulationUniforms(
        float timeSeconds,
        std::uint32_t cascadeIndex,
        std::uint32_t stageIndex,
        std::uint32_t stageAxis) const;
    void dispatchSpectrumUpdate(SDL_GPUCommandBuffer* commandBuffer, const WaterSimulationUniforms& uniforms);
    void dispatchInitializeSpectrum(SDL_GPUCommandBuffer* commandBuffer, const WaterSimulationUniforms& uniforms);
    void dispatchFftStages(SDL_GPUCommandBuffer* commandBuffer, float timeSeconds);
    void dispatchBuildMaps(SDL_GPUCommandBuffer* commandBuffer, const WaterSimulationUniforms& uniforms);

    SDL_GPUGraphicsPipeline* m_mainPipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_bridgePipeline = nullptr;
    SDL_GPUComputePipeline* m_initializeSpectrumPipeline = nullptr;
    SDL_GPUComputePipeline* m_spectrumUpdatePipeline = nullptr;
    SDL_GPUComputePipeline* m_fftStagePipeline = nullptr;
    SDL_GPUComputePipeline* m_buildMapsPipeline = nullptr;
    MeshResources m_mesh{};
    MeshResources m_bridgeMesh{};
    MeshRange m_bridgeMeshRange{};
    MeshRange m_coarseBridgeMeshRange{};
    InstanceResources m_instances{};
    InstanceResources m_bridgeInstances{};
    InstanceResources m_coarseBridgeInstances{};
    SDL_GPUBuffer* m_bridgeIndirectBuffer = nullptr;
    SDL_GPUTransferBuffer* m_bridgeIndirectTransferBuffer = nullptr;
    std::array<SDL_GPUIndexedIndirectDrawCommand, 2> m_bridgeIndirectCommands{};
    std::uint32_t m_bridgeIndirectCommandCount = 0;
    WorkingBufferResources m_workingBuffers{};
    SDL_GPUTexture* m_displacementTexture = nullptr;
    SDL_GPUTexture* m_slopeTexture = nullptr;
    SDL_GPUTexture* m_foamHistoryReadTexture = nullptr;
    SDL_GPUTexture* m_foamHistoryWriteTexture = nullptr;
    SDL_GPUSampler* m_waterSampler = nullptr;
    WaterSettings m_settings{};
    bool m_initialSpectrumDirty = true;
    bool m_hasValidFoamHistory = false;
};
