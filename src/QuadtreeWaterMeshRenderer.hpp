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
    void rebuildMeshLods();

    void addLeaf(
        const WorldGridQuadtreeLeafId& leafId,
        const Position& leafOrigin,
        double leafSizeMeters,
        std::uint8_t quadtreeLodHint,
        std::uint8_t waterMeshLod,
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

    [[nodiscard]] std::uint32_t instanceCountForLod(std::uint32_t lodIndex) const;
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

    struct MeshLodResources
    {
        SDL_GPUBuffer* vertexBuffer = nullptr;
        SDL_GPUTransferBuffer* vertexTransferBuffer = nullptr;
        SDL_GPUBuffer* indexBuffer = nullptr;
        SDL_GPUTransferBuffer* indexTransferBuffer = nullptr;
        std::uint32_t vertexResolution = 0;
        std::uint32_t indexCount = 0;
    };

    struct InstanceLodResources
    {
        SDL_GPUBuffer* instanceBuffer = nullptr;
        SDL_GPUTransferBuffer* instanceTransferBuffer = nullptr;
        std::array<InstanceData, AppConfig::Water::kMaxWaterInstancesPerLod> instances{};
        std::uint32_t instanceCount = 0;
    };

    static_assert(sizeof(InstanceData) % 16 == 0);

    [[nodiscard]] static std::uint32_t packMetadata(
        std::uint8_t quadtreeLodHint,
        std::uint8_t waterMeshLod,
        std::uint32_t bandMask);

    void createPipeline(const std::filesystem::path& shaderDirectory);
    void createMeshLods();
    void createMeshLod(std::uint32_t lodIndex, std::uint32_t vertexResolution);
    void destroyMeshLods();
    void createInstanceBuffers();
    void destroyInstanceBuffers();

    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;
    std::array<MeshLodResources, AppConfig::Water::kMeshLodCount> m_meshLods{};
    std::array<InstanceLodResources, AppConfig::Water::kMeshLodCount> m_instanceLods{};
    WaterSettings m_settings{};
};
