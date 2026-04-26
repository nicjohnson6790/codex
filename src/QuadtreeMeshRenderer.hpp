#pragma once

#include "EngineRendererBase.hpp"
#include "LightingSystem.hpp"
#include "Position.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <filesystem>

class QuadtreeMeshRenderer : private EngineRendererBase
{
public:
    // Per-frame shader constants shared by every terrain draw.
    struct TerrainUniforms
    {
        glm::mat4 viewProjection{1.0f};
        glm::vec4 sunDirectionIntensity{0.0f, 1.0f, 0.0f, 1.0f};
        glm::vec4 sunColorAmbient{1.0f, 1.0f, 1.0f, 0.2f};
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

    float* getCopyBuffer(std::uint16_t slice);

    // Queues one quadtree leaf instance for drawing, using the given heightmap slice.
    void addLeaf(const WorldGridQuadtreeLeafId &leafId, std::uint16_t sliceIndex);

    // Uploads staged instance, indirect, and heightmap data into GPU buffers.
    void upload(SDL_GPUCopyPass *copyPass);

    // Issues the terrain draws for all queued leaf instances.
    void render(SDL_GPURenderPass *renderPass, SDL_GPUCommandBuffer *commandBuffer, const glm::mat4 &viewProjection, const LightingSystem &lightingSystem) const;

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

    static_assert(sizeof(InstanceData) == 16, "Terrain instance data must stay 16 bytes.");
    static_assert(offsetof(InstanceData, position) == 0, "Terrain instance position must start at offset 0.");
    static_assert(offsetof(InstanceData, packedMetadata) == 12, "Terrain packed metadata must stay at offset 12.");

    [[nodiscard]] static std::uint32_t packMetadata(std::uint16_t sliceIndex, std::uint8_t scalePow);

    // Creates the terrain graphics pipeline and loads the terrain shaders.
    void createPipeline(const std::filesystem::path &shaderDirectory);

    // Builds the static mesh buffers for the reusable terrain patch.
    void createStaticMeshResources();

    // Convenience helper for filling SDL's indexed-indirect draw struct.
    [[nodiscard]] static SDL_GPUIndexedIndirectDrawCommand makeDrawCommand(
        std::uint32_t indexCount,
        std::uint32_t instanceCount,
        std::uint32_t firstIndex,
        std::int32_t vertexOffset,
        std::uint32_t firstInstance);

    // Pipeline object for the terrain pass.
    SDL_GPUGraphicsPipeline *m_pipeline = nullptr;

    // Static patch mesh buffers plus their one-time upload buffers.
    SDL_GPUBuffer *m_vertexBuffer = nullptr;
    SDL_GPUTransferBuffer *m_vertexTransferBuffer = nullptr;
    SDL_GPUBuffer *m_indexBuffer = nullptr;
    SDL_GPUTransferBuffer *m_indexTransferBuffer = nullptr;

    // Per-frame instance data buffer plus staging buffer.
    SDL_GPUBuffer *m_instanceBuffer = nullptr;
    SDL_GPUTransferBuffer *m_instanceTransferBuffer = nullptr;

    // Indirect draw-command buffer plus staging buffer.
    SDL_GPUBuffer *m_indirectBuffer = nullptr;
    SDL_GPUTransferBuffer *m_indirectTransferBuffer = nullptr;

    // GPU heightmap slice storage plus the staging buffer used to upload regenerated slices.
    SDL_GPUBuffer *m_heightmapBuffer = nullptr;
    SDL_GPUTransferBuffer *m_heightmapTransferBuffer = nullptr;

    // Cached index count for the reusable terrain patch mesh.
    std::uint32_t m_mainIndexCount = 0;

    std::array<InstanceData, AppConfig::Terrain::kHeightmapSliceCapacity> m_instanceData{};
    std::uint16_t m_instanceCount = 0;
    std::uint16_t m_uploadSlice = UINT16_MAX;
};
