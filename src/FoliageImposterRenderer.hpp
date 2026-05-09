#pragma once

#include "EngineRendererBase.hpp"
#include "FoliageTypes.hpp"
#include "assets/RuntimeAssetReader.hpp"

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

class LightingSystem;

class FoliageImposterRenderer : private EngineRendererBase
{
public:
    struct Vertex
    {
        glm::vec2 corner{ 0.0f };
        glm::vec2 uv0{ 0.0f };
    };

    struct Uniforms
    {
        glm::mat4 viewProjection{ 1.0f };
        glm::vec4 cameraPositionAndTreeClassCount{ 0.0f };
    };

    struct alignas(16) DrawMetadataGpu
    {
        glm::vec4 pageOriginAndTerrainSize{ 0.0f };
        glm::vec4 terrainOriginAndSlice{ 0.0f };
        glm::uvec4 seedData{ 0u };
    };

    struct alignas(16) TreeClassGpu
    {
        glm::vec4 centerAndHalfWidth{ 0.0f };
        glm::vec4 verticalExtentsAndLayerBase{ 0.0f };
    };

    struct FragmentUniforms
    {
        glm::vec4 sunDirectionIntensity{ 0.0f };
        glm::vec4 sunColorAmbient{ 0.0f };
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
        const LightingSystem& lightingSystem,
        SDL_GPUBuffer* terrainHeightmapBuffer) const;

    [[nodiscard]] std::uint32_t drawCount() const { return m_drawCount; }
    [[nodiscard]] std::uint32_t emittedInstanceCount() const { return m_emittedInstanceCount; }
    [[nodiscard]] SDL_GPUBuffer* pagePoolBuffer() const { return m_pagePoolBuffer; }

private:
    [[nodiscard]] static SDL_GPUIndexedIndirectDrawCommand makeDrawCommand(
        std::uint32_t indexCount,
        std::uint32_t instanceCount,
        std::uint32_t firstIndex,
        std::int32_t vertexOffset,
        std::uint32_t firstInstance);
    void createPipeline(const std::filesystem::path& shaderDirectory);
    void createDepthPrepassPipeline(const std::filesystem::path& shaderDirectory);
    void loadRuntimeAssets();
    void createPagePoolBuffers();
    void createIndirectAndDrawMetadataBuffers();
    void createQuadBuffers();
    void createMaterialSampler();
    void createClassMetadataBuffer();
    [[nodiscard]] SDL_GPUTexture* createImposterTextureArray(
        const RuntimeAssets::LoadedTexBinView& texBin,
        std::span<const std::uint32_t> textureIndices,
        RuntimeAssets::TextureFormat expectedFormat,
        const char* label) const;
    static SDL_GPUTextureFormat textureFormatFromRuntimeFormat(RuntimeAssets::TextureFormat format);

    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_depthPrepassPipeline = nullptr;
    SDL_GPUSampler* m_materialSampler = nullptr;
    SDL_GPUTexture* m_imposterColorTextureArray = nullptr;
    SDL_GPUTexture* m_imposterNormalTextureArray = nullptr;
    SDL_GPUBuffer* m_quadVertexBuffer = nullptr;
    SDL_GPUTransferBuffer* m_quadVertexTransferBuffer = nullptr;
    SDL_GPUBuffer* m_quadIndexBuffer = nullptr;
    SDL_GPUTransferBuffer* m_quadIndexTransferBuffer = nullptr;
    SDL_GPUBuffer* m_pagePoolBuffer = nullptr;
    SDL_GPUBuffer* m_drawMetadataBuffer = nullptr;
    SDL_GPUTransferBuffer* m_drawMetadataTransferBuffer = nullptr;
    SDL_GPUBuffer* m_treeClassBuffer = nullptr;
    SDL_GPUTransferBuffer* m_treeClassTransferBuffer = nullptr;
    SDL_GPUBuffer* m_indirectBuffer = nullptr;
    SDL_GPUTransferBuffer* m_indirectTransferBuffer = nullptr;

    std::array<DrawMetadataGpu, AppConfig::Foliage::kMarkerPageDrawCapacity> m_drawMetadata{};
    std::array<SDL_GPUIndexedIndirectDrawCommand, AppConfig::Foliage::kMarkerPageDrawCapacity> m_drawCommands{};
    std::array<std::uint32_t, FoliageConfig::kPagePoolCapacity> m_pageDrawSlots{};
    std::array<std::uint8_t, AppConfig::Foliage::kMarkerPageDrawCapacity> m_drawTerrainScalePows{};
    std::vector<TreeClassGpu> m_treeClassesGpu;
    std::vector<std::uint32_t> m_imposterColorTextureIndices;
    std::vector<std::uint32_t> m_imposterNormalTextureIndices;
    std::uint32_t m_activeTreeClassCount = 0u;
    std::uint32_t m_drawCount = 0;
    std::uint32_t m_emittedInstanceCount = 0;
};
