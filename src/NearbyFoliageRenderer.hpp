#pragma once

#include "EngineRendererBase.hpp"
#include "FoliageTypes.hpp"
#include "SubmittedGpuFence.hpp"

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>

class NearbyFoliageRenderer : private EngineRendererBase
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

    struct alignas(16) DrawInstanceGpu
    {
        glm::vec4 pageOriginAndSlice{ 0.0f };
        glm::vec4 localOffsetAndMesh{ 0.0f };
    };

    NearbyFoliageRenderer() = default;
    ~NearbyFoliageRenderer() = default;

    NearbyFoliageRenderer(const NearbyFoliageRenderer&) = delete;
    NearbyFoliageRenderer& operator=(const NearbyFoliageRenderer&) = delete;

    void initialize(
        SDL_GPUDevice* device,
        SDL_GPUTextureFormat colorFormat,
        SDL_GPUTextureFormat depthFormat,
        const std::filesystem::path& shaderDirectory);
    void shutdown();

    void setActiveCamera(const Position& cameraPosition);
    void beginFrame(std::uint64_t frameIndex);
    void collectCompletedDecodedPages();

    [[nodiscard]] bool makeResident(
        const WorldGridQuadtreeLeafId& pageKey,
        const FoliageReadyPageInfo& sourcePageInfo,
        std::uint64_t frameIndex);
    void addNearbyInstancesForPage(
        const WorldGridQuadtreeLeafId& pageKey,
        std::uint16_t terrainSliceIndex,
        const Position& nearCenter,
        float nearRadiusMeters);

    void upload(SDL_GPUCopyPass* copyPass);
    void dispatchDecodedPageExpansions(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUBuffer* sourcePagePoolBuffer);
    void queueDecodedPageDownloads(SDL_GPUCopyPass* copyPass);
    void attachSubmittedFence(const std::shared_ptr<SubmittedGpuFence>& fence);
    void render(
        SDL_GPURenderPass* renderPass,
        SDL_GPUCommandBuffer* commandBuffer,
        const glm::mat4& viewProjection,
        SDL_GPUBuffer* terrainHeightmapBuffer) const;

    [[nodiscard]] std::uint32_t drawCount() const { return m_drawCount; }
    [[nodiscard]] std::uint32_t drawCallCount() const { return m_drawCount > 0u ? 1u : 0u; }
    [[nodiscard]] std::uint32_t emittedInstanceCount() const { return m_drawCount; }
    [[nodiscard]] std::uint32_t decodedResidentCount() const;
    [[nodiscard]] std::uint32_t decodedPendingCount() const;

private:
    struct alignas(16) DecodeRequestGpu
    {
        glm::uvec4 sourcePageData{ 0u };
    };

    struct DecodedPageEntry
    {
        WorldGridQuadtreeLeafId key{};
        std::uint16_t sourcePageIndex = 0u;
        std::uint16_t liveCount = 0u;
        std::uint32_t contentVersion = 0u;
        std::uint32_t layoutVersion = 0u;
        std::array<DecodedNearbyFoliageInstance, FoliageConfig::kCandidateSlotCount> instances{};
        bool valid = false;
        bool readbackPending = false;
        std::uint64_t lastUsedFrame = 0u;
    };

    struct PendingDecodeRequest
    {
        std::uint16_t entryIndex = FoliageConfig::kNearbyDecodedPageLruCapacity;
        WorldGridQuadtreeLeafId key{};
        FoliageReadyPageInfo sourcePageInfo{};
    };

    struct PendingReadback
    {
        SDL_GPUTransferBuffer* transferBuffer = nullptr;
        std::shared_ptr<SubmittedGpuFence> fence{};
        std::uint16_t entryIndex = FoliageConfig::kNearbyDecodedPageLruCapacity;
        WorldGridQuadtreeLeafId key{};
        std::uint32_t contentVersion = 0u;
        std::uint16_t liveCount = 0u;
    };

    static_assert(sizeof(DecodedNearbyFoliageInstance) == 16, "Nearby foliage decoded instance layout must stay 16 bytes.");
    static constexpr std::uint32_t kDecodedPageByteSize =
        sizeof(DecodedNearbyFoliageInstance) * FoliageConfig::kCandidateSlotCount;

    [[nodiscard]] static SDL_GPUIndirectDrawCommand makeDrawCommand(
        std::uint32_t vertexCount,
        std::uint32_t instanceCount,
        std::uint32_t firstVertex,
        std::uint32_t firstInstance);
    void createPipeline(const std::filesystem::path& shaderDirectory);
    void createDecodeComputePipeline(const std::filesystem::path& shaderDirectory);
    void createMarkerVertexBuffer();
    void createDecodeBuffers();
    void createDrawBuffers();
    void resetTransientState();
    [[nodiscard]] std::uint16_t findEntryIndex(const WorldGridQuadtreeLeafId& pageKey) const;
    [[nodiscard]] bool entryMatchesSource(
        const DecodedPageEntry& entry,
        const WorldGridQuadtreeLeafId& pageKey,
        const FoliageReadyPageInfo& sourcePageInfo) const;
    [[nodiscard]] bool entryIsHintedThisFrame(const WorldGridQuadtreeLeafId& pageKey) const;
    void addTopologyHint(const WorldGridQuadtreeLeafId& pageKey);
    [[nodiscard]] std::uint16_t findReusableEntryIndex() const;

    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;
    SDL_GPUComputePipeline* m_decodeComputePipeline = nullptr;
    SDL_GPUBuffer* m_markerVertexBuffer = nullptr;
    SDL_GPUTransferBuffer* m_markerVertexTransferBuffer = nullptr;
    SDL_GPUBuffer* m_decodeRequestBuffer = nullptr;
    SDL_GPUTransferBuffer* m_decodeRequestTransferBuffer = nullptr;
    SDL_GPUBuffer* m_decodedOutputBuffer = nullptr;
    SDL_GPUTransferBuffer* m_decodedZeroTransferBuffer = nullptr;
    SDL_GPUBuffer* m_drawInstanceBuffer = nullptr;
    SDL_GPUTransferBuffer* m_drawInstanceTransferBuffer = nullptr;
    SDL_GPUBuffer* m_indirectBuffer = nullptr;
    SDL_GPUTransferBuffer* m_indirectTransferBuffer = nullptr;

    std::array<DecodedPageEntry, FoliageConfig::kNearbyDecodedPageLruCapacity> m_decodedPages{};
    std::array<PendingDecodeRequest, FoliageConfig::kNearbyDecodeDispatchBudgetPerFrame> m_pendingDecodeRequests{};
    std::array<PendingDecodeRequest, FoliageConfig::kNearbyDecodeDispatchBudgetPerFrame> m_lastDispatchedDecodeRequests{};
    std::array<DecodeRequestGpu, FoliageConfig::kNearbyDecodeDispatchBudgetPerFrame> m_decodeRequestsGpu{};
    std::array<PendingReadback, FoliageConfig::kNearbyReadbackSlotCount> m_pendingReadbacks{};
    std::array<std::uint16_t, FoliageConfig::kNearbyReadbackSlotCount> m_pendingFenceReadbackSlots{};
    std::array<WorldGridQuadtreeLeafId, 9> m_topologyHints{};
    std::array<DrawInstanceGpu, AppConfig::Foliage::kNearbyMarkerInstanceCapacity> m_drawInstances{};
    std::uint16_t m_pendingDecodeCount = 0u;
    std::uint16_t m_lastDispatchedDecodeCount = 0u;
    std::uint16_t m_pendingFenceReadbackCount = 0u;
    std::uint16_t m_topologyHintCount = 0u;
    std::uint32_t m_drawCount = 0u;
    std::uint64_t m_frameIndex = 0u;
};
