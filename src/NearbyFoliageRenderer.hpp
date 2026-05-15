#pragma once

#include "EngineRendererBase.hpp"
#include "FoliageTypes.hpp"
#include "RenderTypes.hpp"
#include "SubmittedGpuFence.hpp"
#include "assets/RuntimeAssetReader.hpp"

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

class LightingSystem;
class SkyboxRenderer;

class NearbyFoliageRenderer : private EngineRendererBase
{
public:
    struct Vertex
    {
        glm::vec3 position{ 0.0f };
        glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
        glm::vec4 tangent{ 1.0f, 0.0f, 0.0f, 1.0f };
        glm::vec2 uv0{ 0.0f };
    };

    struct VertexUniforms
    {
        glm::mat4 viewProjection{ 1.0f };
    };

    struct FragmentUniforms
    {
        glm::vec4 sunDirectionIntensity{ 0.0f, 1.0f, 0.0f, 1.0f };
        glm::vec4 sunColorAmbient{ 1.0f };
        glm::vec4 shadingParams0{ 0.04f, 1.0f, 0.35f, 0.0f };
        glm::mat4 skyRotation{ 1.0f };
        glm::vec4 atmosphereParams{ 0.0f };
        glm::vec4 sunDirectionTimeOfDay{ 0.0f };
    };

    struct alignas(16) DrawInstanceGpu
    {
        glm::vec4 pageOriginAndSlice{ 0.0f };
        glm::vec4 localOffsetAndMesh{ 0.0f };
        glm::vec4 rotationAndReserved{ 0.0f };
    };

    struct CpuResidentPageView
    {
        WorldGridQuadtreeLeafId pageKey{};
        std::uint16_t liveCount = 0u;
        std::uint32_t contentVersion = 0u;
        std::span<const DecodedNearbyFoliageInstance> instances{};
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
    void setActiveCamera(
        const Position& cameraPosition,
        const glm::dvec3& cameraForward,
        const glm::dvec3& cameraUp,
        Extent2D viewportExtent);
    void clear();
    void collectCompletedDecodedPages();

    [[nodiscard]] std::uint16_t makeResident(
        const WorldGridQuadtreeLeafId& pageKey,
        const FoliageReadyPageInfo& sourcePageInfo);
    void addNearbyInstancesForPage(
        const WorldGridQuadtreeLeafId& pageKey,
        std::uint16_t decodedPageIndex,
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
        const LightingSystem& lightingSystem,
        const SkyboxRenderer& skyboxRenderer,
        SDL_GPUBuffer* terrainHeightmapBuffer) const;

    [[nodiscard]] std::uint32_t drawCount() const { return m_drawCount; }
    [[nodiscard]] std::uint32_t drawCallCount() const;
    [[nodiscard]] std::uint32_t emittedInstanceCount() const { return m_drawCount; }
    [[nodiscard]] std::uint32_t decodedResidentCount() const;
    [[nodiscard]] std::uint32_t decodedPendingCount() const;
    [[nodiscard]] bool tryGetCpuResidentPage(
        const WorldGridQuadtreeLeafId& pageKey,
        CpuResidentPageView& view) const;

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
        std::uint8_t lruAge = 255u;
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

    struct alignas(16) MaterialGpu
    {
        glm::uvec4 layers0{ 0u };
        glm::uvec4 layers1{ 0u };
        glm::vec4 params{ 0.0f };
    };

    struct alignas(16) DrawMetadataGpu
    {
        glm::uvec4 instanceOffsetAndMaterial{ 0u };
        glm::vec4 classCenterAndLodCenter{ 0.0f };
    };

    [[nodiscard]] static SDL_GPUIndexedIndirectDrawCommand makeDrawCommand(
        std::uint32_t indexCount,
        std::uint32_t instanceCount,
        std::uint32_t firstIndex,
        std::int32_t vertexOffset);
    void createPipeline(const std::filesystem::path& shaderDirectory);
    void createDepthPrepassPipeline(const std::filesystem::path& shaderDirectory);
    void createDecodeComputePipeline(const std::filesystem::path& shaderDirectory);
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
    void loadRuntimeAssets();
    void createMaterialSampler();
    void createDefaultTextures();
    void createMeshBuffers(
        std::span<const Vertex> vertices,
        std::span<const std::uint32_t> indices);
    [[nodiscard]] SDL_GPUTexture* createTexture2d(
        SDL_GPUTextureFormat format,
        std::uint32_t width,
        std::uint32_t height,
        const std::byte* bytes,
        std::size_t byteCount) const;
    void createMaterialResources(
        const RuntimeAssets::LoadedTexBinView& texBin,
        const RuntimeAssets::LoadedAssetBinView& assetBin,
        const std::unordered_map<std::uint32_t, std::uint32_t>& usedBaseColorTextures,
        const std::unordered_map<std::uint32_t, std::uint32_t>& usedNormalTextures,
        const std::unordered_map<std::uint32_t, std::uint32_t>& usedRoughnessTextures,
        const std::unordered_map<std::uint32_t, std::uint32_t>& usedSpecularTextures,
        const std::unordered_map<std::uint32_t, std::uint32_t>& usedAoTextures,
        const std::unordered_map<std::uint32_t, std::uint32_t>& usedSubsurfaceTextures);
    [[nodiscard]] std::vector<std::byte> resampleTextureRgba(
        const std::byte* sourcePixels,
        std::uint32_t sourceWidth,
        std::uint32_t sourceHeight,
        std::uint32_t targetWidth,
        std::uint32_t targetHeight) const;
    [[nodiscard]] bool sphereMayIntersectView(
        const glm::vec3& center,
        float radius) const;

    struct LoadedMaterialGpu
    {
        std::uint32_t baseColorLayer = 0u;
        std::uint32_t normalLayer = 0u;
        std::uint32_t roughnessLayer = 0u;
        std::uint32_t specularLayer = 0u;
        std::uint32_t aoLayer = 0u;
        std::uint32_t subsurfaceLayer = 0u;
        float alphaCutoff = 0.0f;
        std::uint32_t flags = 0u;
    };

    struct LoadedDrawPart
    {
        std::uint32_t indexCount = 0u;
        std::uint32_t firstIndex = 0u;
        std::int32_t vertexOffset = 0;
        std::uint32_t materialIndex = 0u;
        glm::vec2 classCenterXz{ 0.0f };
        glm::vec2 lodCenterXz{ 0.0f };
    };

    struct LoadedLodAsset
    {
        std::string name;
        std::vector<LoadedDrawPart> drawParts;
        glm::vec3 boundsCenter{ 0.0f };
        float boundsRadius = 0.0f;
    };

    static constexpr std::uint32_t kNearbyTreeClassCount = 16u;
    static constexpr std::uint32_t kNearbyLodCount = 3u;
    static constexpr std::uint32_t kNearbyDrawGroupCount = kNearbyTreeClassCount * kNearbyLodCount;

    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_depthPrepassPipeline = nullptr;
    SDL_GPUComputePipeline* m_decodeComputePipeline = nullptr;
    SDL_GPUBuffer* m_meshVertexBuffer = nullptr;
    SDL_GPUTransferBuffer* m_meshVertexTransferBuffer = nullptr;
    SDL_GPUBuffer* m_meshIndexBuffer = nullptr;
    SDL_GPUTransferBuffer* m_meshIndexTransferBuffer = nullptr;
    SDL_GPUBuffer* m_decodeRequestBuffer = nullptr;
    SDL_GPUTransferBuffer* m_decodeRequestTransferBuffer = nullptr;
    SDL_GPUBuffer* m_decodedOutputBuffer = nullptr;
    SDL_GPUTransferBuffer* m_decodedZeroTransferBuffer = nullptr;
    SDL_GPUBuffer* m_drawInstanceBuffer = nullptr;
    SDL_GPUTransferBuffer* m_drawInstanceTransferBuffer = nullptr;
    SDL_GPUBuffer* m_indirectBuffer = nullptr;
    SDL_GPUTransferBuffer* m_indirectTransferBuffer = nullptr;
    SDL_GPUBuffer* m_drawMetadataBuffer = nullptr;
    SDL_GPUTransferBuffer* m_drawMetadataTransferBuffer = nullptr;
    SDL_GPUBuffer* m_materialBuffer = nullptr;
    SDL_GPUTransferBuffer* m_materialTransferBuffer = nullptr;
    SDL_GPUSampler* m_materialSampler = nullptr;
    SDL_GPUTexture* m_baseColorTextureArray = nullptr;
    SDL_GPUTexture* m_normalTextureArray = nullptr;
    SDL_GPUTexture* m_roughnessTextureArray = nullptr;
    SDL_GPUTexture* m_specularTextureArray = nullptr;
    SDL_GPUTexture* m_aoTextureArray = nullptr;
    SDL_GPUTexture* m_subsurfaceTextureArray = nullptr;

    std::array<DecodedPageEntry, FoliageConfig::kNearbyDecodedPageLruCapacity> m_decodedPages{};
    std::array<PendingDecodeRequest, FoliageConfig::kNearbyDecodeDispatchBudgetPerFrame> m_pendingDecodeRequests{};
    std::array<PendingDecodeRequest, FoliageConfig::kNearbyDecodeDispatchBudgetPerFrame> m_lastDispatchedDecodeRequests{};
    std::array<DecodeRequestGpu, FoliageConfig::kNearbyDecodeDispatchBudgetPerFrame> m_decodeRequestsGpu{};
    std::array<PendingReadback, FoliageConfig::kNearbyReadbackSlotCount> m_pendingReadbacks{};
    std::array<std::uint16_t, FoliageConfig::kNearbyReadbackSlotCount> m_pendingFenceReadbackSlots{};
    std::array<WorldGridQuadtreeLeafId, 9> m_topologyHints{};
    std::array<DrawInstanceGpu, AppConfig::Foliage::kNearbyMarkerInstanceCapacity> m_drawInstances{};
    std::array<DrawInstanceGpu, AppConfig::Foliage::kNearbyMarkerInstanceCapacity> m_groupedDrawInstances{};
    std::array<std::uint32_t, kNearbyDrawGroupCount> m_groupFirstInstances{};
    std::array<std::uint32_t, kNearbyDrawGroupCount> m_groupInstanceCounts{};
    std::vector<LoadedMaterialGpu> m_loadedMaterials;
    std::vector<MaterialGpu> m_materialGpuRecords;
    std::vector<DrawMetadataGpu> m_drawMetadataGpu;
    std::vector<SDL_GPUIndexedIndirectDrawCommand> m_drawCommands;
    std::array<std::array<LoadedLodAsset, kNearbyLodCount>, kNearbyTreeClassCount> m_loadedClassLods{};
    std::uint32_t m_activeDrawCommandCount = 0u;
    std::uint32_t m_activeTreeClassCount = 0u;
    std::uint16_t m_pendingDecodeCount = 0u;
    std::uint16_t m_lastDispatchedDecodeCount = 0u;
    std::uint16_t m_pendingFenceReadbackCount = 0u;
    std::uint16_t m_topologyHintCount = 0u;
    std::uint32_t m_drawCount = 0u;
    glm::vec3 m_cameraForward{ 0.0f, 0.0f, -1.0f };
    glm::vec3 m_cameraRight{ 1.0f, 0.0f, 0.0f };
    float m_tanHalfHorizontalFov = 1.0f;
    bool m_runtimeAssetsLoaded = false;
};
