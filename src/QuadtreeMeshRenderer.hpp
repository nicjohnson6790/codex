#pragma once

#include "EngineRendererBase.hpp"
#include "HeightmapNoiseGenerator.hpp"
#include "LightingSystem.hpp"
#include "Position.hpp"
#include "WorldGridQuadtreeHeightmapManager.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

class QuadtreeMeshRenderer : private EngineRendererBase
{
public:
    struct GeneratedHeightmapExtents
    {
        WorldGridQuadtreeLeafId leafId{};
        std::uint16_t sliceIndex = 0;
        HeightmapExtents extents{};
    };

    // Per-frame shader constants shared by every terrain draw.
    struct TerrainUniforms
    {
        glm::mat4 viewProjection{1.0f};
        glm::vec4 sunDirectionIntensity{0.0f, 1.0f, 0.0f, 1.0f};
        glm::vec4 sunColorAmbient{1.0f, 1.0f, 1.0f, 0.2f};
        glm::vec4 terrainHeightParams{0.0f, static_cast<float>(AppConfig::Terrain::kHighDetailAmplitude), 0.0f, 0.0f};
    };

    QuadtreeMeshRenderer() = default;
    ~QuadtreeMeshRenderer() = default;

    QuadtreeMeshRenderer(const QuadtreeMeshRenderer &) = delete;
    QuadtreeMeshRenderer &operator=(const QuadtreeMeshRenderer &) = delete;

    void initialize(
        SDL_GPUDevice *device,
        SDL_GPUTextureFormat colorFormat,
        SDL_GPUTextureFormat depthFormat,
        const std::filesystem::path &shaderDirectory);
    void shutdown();

    // Clears the queued instance list for the next frame.
    void clear();

    // Sets the camera used to convert world Positions into camera-local coordinates.
    void setActiveCamera(const Position& cameraPosition);
    void setTerrainHeightParams(float baseHeight, float heightAmplitude);

    [[nodiscard]] bool queueHeightmapGeneration(
        const WorldGridQuadtreeLeafId& leafId,
        std::uint16_t sliceIndex,
        const TerrainNoiseSettings& settings);

    // Queues one quadtree leaf instance for drawing, using the given heightmap slice.
    void addLeaf(const WorldGridQuadtreeLeafId &leafId, std::uint16_t sliceIndex);
    void addBridge(const WorldGridQuadtreeLeafId& leafId, std::uint16_t sliceIndex, std::uint8_t edgeIndex);
    void addCoarseBridge(const WorldGridQuadtreeLeafId& leafId, std::uint16_t sliceIndex, std::uint8_t edgeIndex);

    // Uploads staged instance and indirect draw data into GPU buffers.
    void upload(SDL_GPUCopyPass *copyPass);

    // Dispatches any queued heightmap compute jobs into the heightmap storage buffer.
    void dispatchHeightmapGenerations(SDL_GPUCommandBuffer* commandBuffer);
    void queueHeightmapExtentsDownload(SDL_GPUCopyPass* copyPass);
    void attachSubmittedFence(SDL_GPUFence* fence);
    void collectCompletedHeightmapExtents(std::vector<GeneratedHeightmapExtents>& completedExtents);

    // Issues the terrain draws for all queued leaf instances.
    void render(SDL_GPURenderPass *renderPass, SDL_GPUCommandBuffer *commandBuffer, const glm::mat4 &viewProjection, const LightingSystem &lightingSystem) const;
    [[nodiscard]] SDL_GPUBuffer* heightmapBuffer() const { return m_heightmapBuffer; }

private:
    // Static grid vertex data for one terrain patch. localCoord is patch space; sampleCoord is height lookup space.
    struct Vertex
    {
        float localCoord[2];
        float sampleCoord[2];
    };

    // Per-instance data consumed by the vertex shader for one leaf draw.
    struct alignas(16) InstanceData
    {
        float position[3]{};
        std::uint32_t packedMetadata = 0;
    };

    struct MeshResources
    {
        SDL_GPUBuffer* vertexBuffer = nullptr;
        SDL_GPUTransferBuffer* vertexTransferBuffer = nullptr;
        SDL_GPUBuffer* indexBuffer = nullptr;
        SDL_GPUTransferBuffer* indexTransferBuffer = nullptr;
        std::uint32_t indexCount = 0;
    };

    struct MeshRange
    {
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
    };

    struct alignas(16) HeightmapGenerationUniforms
    {
        glm::vec4 sampleOriginAndStep{ 0.0f };
        glm::vec4 hillsLayerA{ 0.0f };
        glm::vec4 hillsLayerB{ 0.0f };
        glm::vec4 hillsLayerC{ 0.0f };
        glm::vec4 mediumLayerA{ 0.0f };
        glm::vec4 mediumLayerB{ 0.0f };
        glm::vec4 mediumLayerC{ 0.0f };
        glm::vec4 highLayerA{ 0.0f };
        glm::vec4 highLayerB{ 0.0f };
        glm::vec4 highLayerC{ 0.0f };
        glm::vec4 blendLayerA{ 0.0f };
        glm::vec4 blendLayerB{ 0.0f };
        glm::vec4 blendLayerC{ 0.0f };
        glm::uvec4 dispatchParams{ 0u };
    };

    struct alignas(8) GpuHeightmapExtents
    {
        std::int32_t minHeightCentimeters = 0;
        std::int32_t maxHeightCentimeters = 0;
    };

    struct PendingExtentsReadback
    {
        SDL_GPUTransferBuffer* transferBuffer = nullptr;
        SDL_GPUFence* fence = nullptr;
        std::array<WorldGridQuadtreeLeafId, AppConfig::Terrain::kHeightmapSliceCapacity> leafIds{};
        std::array<std::uint16_t, AppConfig::Terrain::kHeightmapSliceCapacity> sliceIndices{};
        std::uint16_t count = 0;
    };

    static_assert(sizeof(InstanceData) == 16, "Terrain instance data must stay 16 bytes.");
    static_assert(offsetof(InstanceData, position) == 0, "Terrain instance position must start at offset 0.");
    static_assert(offsetof(InstanceData, packedMetadata) == 12, "Terrain packed metadata must stay at offset 12.");
    static_assert(sizeof(HeightmapGenerationUniforms) == 224, "Heightmap generation uniforms must stay tightly packed.");

    [[nodiscard]] static std::uint32_t packMetadata(
        std::uint16_t sliceIndex,
        std::uint8_t scalePow,
        std::uint8_t edgeIndex = 0);

    // Creates the terrain graphics pipelines and loads the terrain shaders.
    void createPipelines(const std::filesystem::path& shaderDirectory);
    void createHeightmapComputePipeline(const std::filesystem::path& shaderDirectory);

    // Builds the static mesh buffers for the reusable terrain meshes.
    void createStaticMeshResources();
    void createMeshResources(
        const std::vector<Vertex>& vertices,
        const std::vector<std::uint32_t>& indices,
        MeshResources& meshResources);
    [[nodiscard]] static float instanceDistanceSquared(const InstanceData& instance);
    static void sortInstances(InstanceData* instances, std::uint16_t instanceCount);

    // Convenience helper for filling SDL's indexed-indirect draw struct.
    [[nodiscard]] static SDL_GPUIndexedIndirectDrawCommand makeDrawCommand(
        std::uint32_t indexCount,
        std::uint32_t instanceCount,
        std::uint32_t firstIndex,
        std::int32_t vertexOffset,
        std::uint32_t firstInstance);

    // Pipeline objects for the terrain pass.
    SDL_GPUGraphicsPipeline* m_mainPipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_bridgePipeline = nullptr;
    SDL_GPUComputePipeline* m_heightmapComputePipeline = nullptr;

    // Static reusable terrain meshes plus their one-time upload buffers.
    MeshResources m_mainMesh{};
    MeshResources m_bridgeMesh{};
    MeshRange m_bridgeMeshRange{};
    MeshRange m_coarseBridgeMeshRange{};

    // Per-frame instance data buffer plus staging buffer.
    SDL_GPUBuffer* m_instanceBuffer = nullptr;
    SDL_GPUTransferBuffer* m_instanceTransferBuffer = nullptr;
    SDL_GPUBuffer* m_bridgeInstanceBuffer = nullptr;
    SDL_GPUTransferBuffer* m_bridgeInstanceTransferBuffer = nullptr;

    // Indirect draw-command buffer plus staging buffer.
    SDL_GPUBuffer* m_indirectBuffer = nullptr;
    SDL_GPUTransferBuffer* m_indirectTransferBuffer = nullptr;
    SDL_GPUBuffer* m_bridgeIndirectBuffer = nullptr;
    SDL_GPUTransferBuffer* m_bridgeIndirectTransferBuffer = nullptr;

    // GPU heightmap slice storage written directly by the compute shader.
    SDL_GPUBuffer* m_heightmapGenerationBuffer = nullptr;
    SDL_GPUTransferBuffer* m_heightmapGenerationTransferBuffer = nullptr;
    SDL_GPUBuffer *m_heightmapBuffer = nullptr;
    SDL_GPUBuffer* m_heightmapExtentsBuffer = nullptr;
    SDL_GPUTransferBuffer* m_heightmapExtentsInitTransferBuffer = nullptr;

    std::array<InstanceData, AppConfig::Terrain::kHeightmapSliceCapacity> m_instanceData{};
    std::array<InstanceData, AppConfig::Terrain::kHeightmapSliceCapacity * 4> m_bridgeInstanceData{};
    std::array<InstanceData, AppConfig::Terrain::kHeightmapSliceCapacity * 4> m_coarseBridgeInstanceData{};
    std::array<SDL_GPUIndexedIndirectDrawCommand, 2> m_bridgeIndirectCommands{};
    std::array<HeightmapGenerationUniforms, AppConfig::Terrain::kHeightmapSliceCapacity> m_pendingHeightmapGenerations{};
    std::array<WorldGridQuadtreeLeafId, AppConfig::Terrain::kHeightmapSliceCapacity> m_pendingGenerationLeafIds{};
    std::array<WorldGridQuadtreeLeafId, AppConfig::Terrain::kHeightmapSliceCapacity> m_lastDispatchedLeafIds{};
    std::array<std::uint16_t, AppConfig::Terrain::kHeightmapSliceCapacity> m_lastDispatchedSlices{};
    static constexpr std::size_t kHeightmapReadbackSlotCount = 8;
    std::array<PendingExtentsReadback, kHeightmapReadbackSlotCount> m_pendingExtentsReadbacks{};
    std::uint16_t m_instanceCount = 0;
    std::uint16_t m_bridgeInstanceCount = 0;
    std::uint16_t m_coarseBridgeInstanceCount = 0;
    std::uint16_t m_bridgeIndirectCommandCount = 0;
    std::uint16_t m_pendingHeightmapGenerationCount = 0;
    std::uint16_t m_lastDispatchedGenerationCount = 0;
    std::uint16_t m_pendingFenceReadbackSlot = UINT16_MAX;
    std::uint16_t m_nextReadbackSlot = 0;
    float m_terrainBaseHeight = 0.0f;
    float m_terrainHeightAmplitude = static_cast<float>(AppConfig::Terrain::kHighDetailAmplitude);
};
