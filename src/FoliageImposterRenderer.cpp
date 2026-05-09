#include "FoliageImposterRenderer.hpp"

#include "AppConfig.hpp"
#include "LightingSystem.hpp"
#include "PerformanceCapture.hpp"

#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace
{
constexpr std::uint32_t kPackedEntrySize = sizeof(FoliagePackedInstance);
constexpr std::uint32_t kPageByteSize = FoliageConfig::kCandidateSlotCount * kPackedEntrySize;
constexpr std::uint32_t kImposterYawViewCount = 8u;
constexpr std::uint32_t kImposterPitchViewCount = 4u;
constexpr std::uint32_t kImposterLayersPerClass = kImposterYawViewCount * kImposterPitchViewCount;
constexpr float kImposterVerticalPaddingScale = 0.08f;
constexpr float kImposterVerticalPaddingMinMeters = 0.15f;
constexpr float kImposterGroundingBiasScale = 0.03f;
constexpr float kImposterGroundingBiasMinMeters = 0.125f;
constexpr std::array<float, kImposterPitchViewCount> kImposterPitchDegrees{
    -5.0f,
    10.0f,
    25.0f,
    40.0f,
};
constexpr float kTau = 6.28318530718f;

bool containsInsensitive(std::string_view haystack, std::string_view needle)
{
    auto lower = [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    };

    return std::search(
        haystack.begin(),
        haystack.end(),
        needle.begin(),
        needle.end(),
        [&](char lhs, char rhs) {
            return lower(static_cast<unsigned char>(lhs)) == lower(static_cast<unsigned char>(rhs));
        }) != haystack.end();
}

std::filesystem::path executableRelativePath(const std::filesystem::path& relativePath)
{
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr)
    {
        throw std::runtime_error(std::string("Failed to resolve executable base path: ") + SDL_GetError());
    }

    return std::filesystem::path(basePath) / relativePath;
}

float horizontalRadiusForBounds(const RuntimeAssets::MeshRecord& mesh)
{
    const float centerX = mesh.boundsSphereCenter[0];
    const float centerZ = mesh.boundsSphereCenter[2];
    const std::array<float, 2> xs{ mesh.boundsMin[0], mesh.boundsMax[0] };
    const std::array<float, 2> zs{ mesh.boundsMin[2], mesh.boundsMax[2] };

    float radius = 0.0f;
    for (const float x : xs)
    {
        for (const float z : zs)
        {
            const float dx = x - centerX;
            const float dz = z - centerZ;
            radius = std::max(radius, std::sqrt((dx * dx) + (dz * dz)));
        }
    }
    return radius;
}

std::uint32_t textureTransferStrideExtent(RuntimeAssets::TextureFormat format, std::uint32_t mipExtent)
{
    if (!RuntimeAssets::TextureFormatIsBlockCompressed(format))
    {
        return mipExtent;
    }

    return std::max<std::uint32_t>(((mipExtent + 3u) / 4u) * 4u, 4u);
}

float imposterDrawDistanceSquared(const FoliageImposterRenderer::DrawMetadataGpu& draw)
{
    const float centerX = draw.pageOriginAndTerrainSize.x + (static_cast<float>(FoliageConfig::kPageSizeMeters) * 0.5f);
    const float centerZ = draw.pageOriginAndTerrainSize.z + (static_cast<float>(FoliageConfig::kPageSizeMeters) * 0.5f);
    return (centerX * centerX) + (centerZ * centerZ);
}
}

void FoliageImposterRenderer::initialize(
    SDL_GPUDevice* device,
    SDL_GPUTextureFormat colorFormat,
    SDL_GPUTextureFormat depthFormat,
    const std::filesystem::path& shaderDirectory)
{
    initializeRendererBase(device, colorFormat, depthFormat);
    createMaterialSampler();
    createQuadBuffers();
    createPagePoolBuffers();
    createIndirectAndDrawMetadataBuffers();
    loadRuntimeAssets();
    createClassMetadataBuffer();
    createPipeline(shaderDirectory);
}

void FoliageImposterRenderer::shutdown()
{
    if (m_pipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
        m_pipeline = nullptr;
    }
    if (m_materialSampler != nullptr)
    {
        SDL_ReleaseGPUSampler(m_device, m_materialSampler);
        m_materialSampler = nullptr;
    }
    if (m_imposterNormalTextureArray != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_imposterNormalTextureArray);
        m_imposterNormalTextureArray = nullptr;
    }
    if (m_imposterColorTextureArray != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_imposterColorTextureArray);
        m_imposterColorTextureArray = nullptr;
    }
    if (m_indirectTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_indirectTransferBuffer);
        m_indirectTransferBuffer = nullptr;
    }
    if (m_indirectBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_indirectBuffer);
        m_indirectBuffer = nullptr;
    }
    if (m_treeClassTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_treeClassTransferBuffer);
        m_treeClassTransferBuffer = nullptr;
    }
    if (m_treeClassBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_treeClassBuffer);
        m_treeClassBuffer = nullptr;
    }
    if (m_drawMetadataTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_drawMetadataTransferBuffer);
        m_drawMetadataTransferBuffer = nullptr;
    }
    if (m_drawMetadataBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_drawMetadataBuffer);
        m_drawMetadataBuffer = nullptr;
    }
    if (m_pagePoolBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_pagePoolBuffer);
        m_pagePoolBuffer = nullptr;
    }
    if (m_quadIndexTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_quadIndexTransferBuffer);
        m_quadIndexTransferBuffer = nullptr;
    }
    if (m_quadIndexBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_quadIndexBuffer);
        m_quadIndexBuffer = nullptr;
    }
    if (m_quadVertexTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_quadVertexTransferBuffer);
        m_quadVertexTransferBuffer = nullptr;
    }
    if (m_quadVertexBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_quadVertexBuffer);
        m_quadVertexBuffer = nullptr;
    }

    m_treeClassesGpu.clear();
    m_imposterColorTextureIndices.clear();
    m_imposterNormalTextureIndices.clear();
    m_activeTreeClassCount = 0u;
    clear();
}

void FoliageImposterRenderer::clear()
{
    m_drawCount = 0;
    m_emittedInstanceCount = 0;
    m_pageDrawSlots.fill(UINT32_MAX);
    m_drawTerrainScalePows.fill(UINT8_MAX);
}

void FoliageImposterRenderer::setActiveCamera(const Position& cameraPosition)
{
    setActiveCameraPosition(cameraPosition);
}

void FoliageImposterRenderer::addPageDraw(const FoliagePageDrawReference& drawReference)
{
    if (drawReference.pageIndex >= FoliageConfig::kPagePoolCapacity ||
        drawReference.liveCount == 0 ||
        m_drawCount >= AppConfig::Foliage::kMarkerPageDrawCapacity)
    {
        return;
    }

    std::uint32_t drawSlot = m_pageDrawSlots[drawReference.pageIndex];
    const bool pageAlreadyQueued = drawSlot != UINT32_MAX;
    if (pageAlreadyQueued &&
        drawReference.terrainScalePow >= m_drawTerrainScalePows[drawSlot])
    {
        return;
    }

    const glm::vec3 pageOrigin = localPositionFromWorldPosition(drawReference.pageOrigin);
    const glm::vec3 terrainOrigin = localPositionFromWorldPosition(drawReference.terrainLeafOrigin);
    const double terrainLeafSizeMeters =
        AppConfig::Quadtree::kMinimumQuadSize * static_cast<double>(1u << drawReference.terrainScalePow);

    if (!pageAlreadyQueued)
    {
        drawSlot = m_drawCount;
        m_pageDrawSlots[drawReference.pageIndex] = drawSlot;
        m_drawCommands[m_drawCount++] = makeDrawCommand(
            6u,
            drawReference.liveCount,
            0u,
            0,
            0u);
        m_emittedInstanceCount += drawReference.liveCount;
    }

    m_drawMetadata[drawSlot] = {
        .pageOriginAndTerrainSize = glm::vec4(
            pageOrigin.x,
            pageOrigin.y,
            pageOrigin.z,
            static_cast<float>(terrainLeafSizeMeters)),
        .terrainOriginAndSlice = glm::vec4(
            terrainOrigin.x,
            terrainOrigin.y,
            terrainOrigin.z,
            static_cast<float>(drawReference.terrainSliceIndex)),
        .seedData = glm::uvec4(drawReference.seed, drawReference.pageIndex, 0u, 0u),
    };
    m_drawTerrainScalePows[drawSlot] = drawReference.terrainScalePow;
}

void FoliageImposterRenderer::upload(SDL_GPUCopyPass* copyPass)
{
    HELLO_PROFILE_SCOPE("FoliageImposterRenderer::Upload");

    if (m_drawCount == 0)
    {
        return;
    }

    std::array<std::uint32_t, AppConfig::Foliage::kMarkerPageDrawCapacity> drawOrder{};
    for (std::uint32_t drawIndex = 0; drawIndex < m_drawCount; ++drawIndex)
    {
        drawOrder[drawIndex] = drawIndex;
    }
    std::sort(
        drawOrder.begin(),
        drawOrder.begin() + m_drawCount,
        [&](std::uint32_t lhs, std::uint32_t rhs) {
            return imposterDrawDistanceSquared(m_drawMetadata[lhs]) < imposterDrawDistanceSquared(m_drawMetadata[rhs]);
        });

    std::array<DrawMetadataGpu, AppConfig::Foliage::kMarkerPageDrawCapacity> sortedMetadata{};
    std::array<SDL_GPUIndexedIndirectDrawCommand, AppConfig::Foliage::kMarkerPageDrawCapacity> sortedCommands{};
    std::array<std::uint8_t, AppConfig::Foliage::kMarkerPageDrawCapacity> sortedTerrainScalePows{};
    m_pageDrawSlots.fill(UINT32_MAX);
    for (std::uint32_t sortedIndex = 0; sortedIndex < m_drawCount; ++sortedIndex)
    {
        const std::uint32_t sourceIndex = drawOrder[sortedIndex];
        sortedMetadata[sortedIndex] = m_drawMetadata[sourceIndex];
        sortedCommands[sortedIndex] = m_drawCommands[sourceIndex];
        sortedTerrainScalePows[sortedIndex] = m_drawTerrainScalePows[sourceIndex];
        const std::uint32_t pageIndex = sortedMetadata[sortedIndex].seedData.y;
        if (pageIndex < m_pageDrawSlots.size())
        {
            m_pageDrawSlots[pageIndex] = sortedIndex;
        }
    }
    std::copy(sortedMetadata.begin(), sortedMetadata.begin() + m_drawCount, m_drawMetadata.begin());
    std::copy(sortedCommands.begin(), sortedCommands.begin() + m_drawCount, m_drawCommands.begin());
    std::copy(sortedTerrainScalePows.begin(), sortedTerrainScalePows.begin() + m_drawCount, m_drawTerrainScalePows.begin());

    void* mappedMetadata = SDL_MapGPUTransferBuffer(m_device, m_drawMetadataTransferBuffer, true);
    std::memcpy(mappedMetadata, m_drawMetadata.data(), sizeof(DrawMetadataGpu) * m_drawCount);
    SDL_UnmapGPUTransferBuffer(m_device, m_drawMetadataTransferBuffer);

    SDL_GPUTransferBufferLocation metadataSource{};
    metadataSource.transfer_buffer = m_drawMetadataTransferBuffer;

    SDL_GPUBufferRegion metadataDestination{};
    metadataDestination.buffer = m_drawMetadataBuffer;
    metadataDestination.size = static_cast<Uint32>(sizeof(DrawMetadataGpu) * m_drawCount);

    SDL_UploadToGPUBuffer(copyPass, &metadataSource, &metadataDestination, true);

    void* mappedIndirect = SDL_MapGPUTransferBuffer(m_device, m_indirectTransferBuffer, true);
    std::memcpy(mappedIndirect, m_drawCommands.data(), sizeof(SDL_GPUIndexedIndirectDrawCommand) * m_drawCount);
    SDL_UnmapGPUTransferBuffer(m_device, m_indirectTransferBuffer);

    SDL_GPUTransferBufferLocation indirectSource{};
    indirectSource.transfer_buffer = m_indirectTransferBuffer;

    SDL_GPUBufferRegion indirectDestination{};
    indirectDestination.buffer = m_indirectBuffer;
    indirectDestination.size = static_cast<Uint32>(sizeof(SDL_GPUIndexedIndirectDrawCommand) * m_drawCount);

    SDL_UploadToGPUBuffer(copyPass, &indirectSource, &indirectDestination, true);
}

void FoliageImposterRenderer::render(
    SDL_GPURenderPass* renderPass,
    SDL_GPUCommandBuffer* commandBuffer,
    const glm::mat4& viewProjection,
    const LightingSystem& lightingSystem,
    SDL_GPUBuffer* terrainHeightmapBuffer) const
{
    HELLO_PROFILE_SCOPE("FoliageImposterRenderer::Render");

    if (m_drawCount == 0 ||
        terrainHeightmapBuffer == nullptr ||
        m_imposterColorTextureArray == nullptr ||
        m_imposterNormalTextureArray == nullptr ||
        m_treeClassBuffer == nullptr ||
        m_activeTreeClassCount == 0u)
    {
        return;
    }

    Uniforms uniforms{};
    uniforms.viewProjection = viewProjection;
    uniforms.cameraPositionAndTreeClassCount = glm::vec4(
        0.0f,
        0.0f,
        0.0f,
        static_cast<float>(m_activeTreeClassCount));

    FragmentUniforms fragmentUniforms{};
    const glm::vec3 sunDirection = lightingSystem.sunDirection();
    fragmentUniforms.sunDirectionIntensity = glm::vec4(sunDirection, lightingSystem.sun().intensity);
    fragmentUniforms.sunColorAmbient = glm::vec4(lightingSystem.sun().color, AppConfig::Terrain::kAmbientLight);

    SDL_BindGPUGraphicsPipeline(renderPass, m_pipeline);

    const SDL_GPUBufferBinding vertexBinding{ m_quadVertexBuffer, 0 };
    SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);
    const SDL_GPUBufferBinding indexBinding{ m_quadIndexBuffer, 0 };
    SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

    SDL_GPUBuffer* vertexStorageBuffers[]{
        m_drawMetadataBuffer,
        m_pagePoolBuffer,
        terrainHeightmapBuffer,
        m_treeClassBuffer,
    };
    SDL_BindGPUVertexStorageBuffers(renderPass, 0, vertexStorageBuffers, 4);

    SDL_GPUTextureSamplerBinding samplerBindings[2]{
        { m_imposterColorTextureArray, m_materialSampler },
        { m_imposterNormalTextureArray, m_materialSampler },
    };
    SDL_BindGPUFragmentSamplers(renderPass, 0, samplerBindings, 2);

    SDL_PushGPUVertexUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));
    SDL_PushGPUFragmentUniformData(commandBuffer, 0, &fragmentUniforms, sizeof(fragmentUniforms));
    SDL_DrawGPUIndexedPrimitivesIndirect(renderPass, m_indirectBuffer, 0, m_drawCount);
}

SDL_GPUIndexedIndirectDrawCommand FoliageImposterRenderer::makeDrawCommand(
    std::uint32_t indexCount,
    std::uint32_t instanceCount,
    std::uint32_t firstIndex,
    std::int32_t vertexOffset,
    std::uint32_t firstInstance)
{
    SDL_GPUIndexedIndirectDrawCommand command{};
    command.num_indices = indexCount;
    command.num_instances = instanceCount;
    command.first_index = firstIndex;
    command.vertex_offset = vertexOffset;
    command.first_instance = firstInstance;
    return command;
}

SDL_GPUTextureFormat FoliageImposterRenderer::textureFormatFromRuntimeFormat(RuntimeAssets::TextureFormat format)
{
    switch (format)
    {
    case RuntimeAssets::TextureFormat::BC3_RGBA_UNORM:
        return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
    case RuntimeAssets::TextureFormat::BC5_RG_UNORM:
        return SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM;
    case RuntimeAssets::TextureFormat::RGBA8_SRGB:
        return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    case RuntimeAssets::TextureFormat::RGBA8_UNORM:
    default:
        return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    }
}

void FoliageImposterRenderer::loadRuntimeAssets()
{
    RuntimeAssets::LoadedMeshBinView meshBin;
    RuntimeAssets::LoadedTexBinView texBin;
    RuntimeAssets::LoadedAssetBinView assetBin;
    std::string error;
    const std::filesystem::path assetRoot = executableRelativePath(TERRAIN_SANDBOX_ASSET_DIR);
    if (!RuntimeAssets::LoadAssetBinFromSDL((assetRoot / "pinetreepack.assetbin").string().c_str(), &assetBin, &error) ||
        !RuntimeAssets::LoadMeshBinFromSDL((assetRoot / "pinetreepack.meshbin").string().c_str(), assetBin, &meshBin, &error) ||
        !RuntimeAssets::LoadTexBinFromSDL((assetRoot / "pinetreepack.texbin").string().c_str(), assetBin, &texBin, &error))
    {
        throw std::runtime_error("Failed to load foliage imposter runtime assets: " + error);
    }

    std::vector<std::uint32_t> selectedAssetIndices;
    selectedAssetIndices.reserve(assetBin.assets.size());
    for (std::uint32_t assetIndex = 0; assetIndex < assetBin.assets.size(); ++assetIndex)
    {
        const RuntimeAssets::AssetRecord& assetRecord = assetBin.assets[assetIndex];
        const char* assetName = assetBin.stringAt(assetRecord.nameOffset);
        if (!containsInsensitive(assetName, "pine"))
        {
            continue;
        }
        if (assetRecord.imposterColorTextureIndex == std::numeric_limits<std::uint32_t>::max() ||
            assetRecord.imposterNormalTextureIndex == std::numeric_limits<std::uint32_t>::max())
        {
            continue;
        }
        selectedAssetIndices.push_back(assetIndex);
    }

    std::sort(
        selectedAssetIndices.begin(),
        selectedAssetIndices.end(),
        [&](std::uint32_t lhs, std::uint32_t rhs) {
            return std::strcmp(
                assetBin.stringAt(assetBin.assets[lhs].nameOffset),
                assetBin.stringAt(assetBin.assets[rhs].nameOffset)) < 0;
        });

    if (selectedAssetIndices.empty())
    {
        throw std::runtime_error("Foliage imposter runtime pack did not contain any pine assets with imposter textures.");
    }
    if (selectedAssetIndices.size() > 16u)
    {
        throw std::runtime_error("Foliage imposter runtime pack contains more tree assets than the packed meshId budget allows.");
    }

    m_activeTreeClassCount = static_cast<std::uint32_t>(selectedAssetIndices.size());
    m_treeClassesGpu.assign(m_activeTreeClassCount, {});
    m_imposterColorTextureIndices.assign(m_activeTreeClassCount, 0u);
    m_imposterNormalTextureIndices.assign(m_activeTreeClassCount, 0u);

    for (std::uint32_t treeClass = 0; treeClass < m_activeTreeClassCount; ++treeClass)
    {
        const RuntimeAssets::AssetRecord& assetRecord = assetBin.assets[selectedAssetIndices[treeClass]];
        m_imposterColorTextureIndices[treeClass] = assetRecord.imposterColorTextureIndex;
        m_imposterNormalTextureIndices[treeClass] = assetRecord.imposterNormalTextureIndex;

        float minY = std::numeric_limits<float>::max();
        float maxY = std::numeric_limits<float>::lowest();
        float minX = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float minZ = std::numeric_limits<float>::max();
        float maxZ = std::numeric_limits<float>::lowest();
        float horizontalRadius = 0.0f;

        for (std::uint32_t meshRefOffset = 0; meshRefOffset < assetRecord.meshRefCount; ++meshRefOffset)
        {
            const RuntimeAssets::MeshRefRecord& meshRef = assetBin.meshRefs[assetRecord.firstMeshRef + meshRefOffset];
            if (meshRef.meshIndex >= meshBin.meshes.size())
            {
                continue;
            }

            const RuntimeAssets::MeshRecord& mesh = meshBin.meshes[meshRef.meshIndex];
            bool billboardMesh = false;
            for (std::uint32_t submeshOffset = 0; submeshOffset < mesh.submeshCount; ++submeshOffset)
            {
                const RuntimeAssets::SubmeshRecord& submesh = meshBin.submeshes[mesh.firstSubmesh + submeshOffset];
                if (submesh.materialIndex >= assetBin.materials.size())
                {
                    continue;
                }
                const RuntimeAssets::MaterialRecord& material = assetBin.materials[submesh.materialIndex];
                if (containsInsensitive(assetBin.stringAt(material.nameOffset), "billboard"))
                {
                    billboardMesh = true;
                    break;
                }
            }
            if (billboardMesh)
            {
                continue;
            }

            minY = std::min(minY, mesh.boundsMin[1]);
            maxY = std::max(maxY, mesh.boundsMax[1]);
            minX = std::min(minX, mesh.boundsMin[0]);
            maxX = std::max(maxX, mesh.boundsMax[0]);
            minZ = std::min(minZ, mesh.boundsMin[2]);
            maxZ = std::max(maxZ, mesh.boundsMax[2]);
            horizontalRadius = std::max(horizontalRadius, horizontalRadiusForBounds(mesh));
        }

        if (minY > maxY || minX > maxX || minZ > maxZ)
        {
            throw std::runtime_error(
                std::string("Foliage imposter asset is missing drawable non-billboard geometry: ") +
                assetBin.stringAt(assetRecord.nameOffset));
        }

        const float centerX = (minX + maxX) * 0.5f;
        const float centerY = (minY + maxY) * 0.5f;
        const float centerZ = (minZ + maxZ) * 0.5f;
        const float assetHeight = maxY - minY;
        const float verticalPadding = std::max(
            assetHeight * kImposterVerticalPaddingScale,
            kImposterVerticalPaddingMinMeters);
        const float groundingBias = std::max(
            assetHeight * kImposterGroundingBiasScale,
            kImposterGroundingBiasMinMeters);
        m_treeClassesGpu[treeClass] = {
            .centerAndHalfWidth = glm::vec4(centerX, centerY, centerZ, std::max(horizontalRadius * 1.08f, 0.5f)),
            .verticalExtentsAndLayerBase = glm::vec4(
                (minY - centerY) - verticalPadding - groundingBias,
                (maxY - centerY) + verticalPadding,
                static_cast<float>(treeClass * kImposterLayersPerClass),
                0.0f),
        };
    }

    m_imposterColorTextureArray = createImposterTextureArray(
        texBin,
        m_imposterColorTextureIndices,
        RuntimeAssets::TextureFormat::BC3_RGBA_UNORM,
        "color");
    m_imposterNormalTextureArray = createImposterTextureArray(
        texBin,
        m_imposterNormalTextureIndices,
        RuntimeAssets::TextureFormat::BC5_RG_UNORM,
        "normal");
}

SDL_GPUTexture* FoliageImposterRenderer::createImposterTextureArray(
    const RuntimeAssets::LoadedTexBinView& texBin,
    std::span<const std::uint32_t> textureIndices,
    RuntimeAssets::TextureFormat expectedFormat,
    const char* label) const
{
    if (textureIndices.empty())
    {
        throw std::runtime_error(std::string("No foliage imposter ") + label + " textures were selected.");
    }

    const RuntimeAssets::TextureRecord& reference = texBin.textures[textureIndices.front()];
    if (reference.format != static_cast<std::uint32_t>(expectedFormat) ||
        reference.dimension != static_cast<std::uint32_t>(RuntimeAssets::TextureDimension::Texture2DArray))
    {
        throw std::runtime_error(std::string("Foliage imposter ") + label + " texture metadata is not the expected array format.");
    }

    const RuntimeAssets::TextureFormat runtimeFormat = static_cast<RuntimeAssets::TextureFormat>(reference.format);
    const SDL_GPUTextureFormat gpuFormat = textureFormatFromRuntimeFormat(runtimeFormat);
    const std::uint32_t arrayLayerCount = static_cast<std::uint32_t>(textureIndices.size()) * reference.layerCount;
    if (!SDL_GPUTextureSupportsFormat(
            m_device,
            gpuFormat,
            SDL_GPU_TEXTURETYPE_2D_ARRAY,
            SDL_GPU_TEXTUREUSAGE_SAMPLER))
    {
        throw std::runtime_error(std::string("SDL GPU device does not support foliage imposter ") + label + " texture format.");
    }

    std::uint64_t totalTransferBytes = 0u;
    for (const std::uint32_t textureIndex : textureIndices)
    {
        const RuntimeAssets::TextureRecord& record = texBin.textures[textureIndex];
        if (record.width != reference.width ||
            record.height != reference.height ||
            record.layerCount != reference.layerCount ||
            record.mipCount != reference.mipCount ||
            record.format != reference.format ||
            record.dimension != reference.dimension)
        {
            throw std::runtime_error(std::string("Foliage imposter ") + label + " textures do not share a consistent array layout.");
        }
        totalTransferBytes += record.dataUncompressedSize;
    }

    SDL_GPUTextureCreateInfo textureInfo{};
    textureInfo.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    textureInfo.format = gpuFormat;
    textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    textureInfo.width = reference.width;
    textureInfo.height = reference.height;
    textureInfo.layer_count_or_depth = arrayLayerCount;
    textureInfo.num_levels = reference.mipCount;
    textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(m_device, &textureInfo);
    if (texture == nullptr)
    {
        throwSdlError(("Failed to create foliage imposter " + std::string(label) + " texture array.").c_str());
    }

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<Uint32>(totalTransferBytes);
    SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
    if (transferBuffer == nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, texture);
        throwSdlError(("Failed to create foliage imposter " + std::string(label) + " transfer buffer.").c_str());
    }

    std::byte* mapped = static_cast<std::byte*>(SDL_MapGPUTransferBuffer(m_device, transferBuffer, false));
    std::uint64_t transferOffset = 0u;
    std::vector<std::uint64_t> textureOffsets(textureIndices.size(), 0u);
    for (std::size_t textureSlot = 0; textureSlot < textureIndices.size(); ++textureSlot)
    {
        const RuntimeAssets::TextureRecord& record = texBin.textures[textureIndices[textureSlot]];
        textureOffsets[textureSlot] = transferOffset;
        const std::byte* sourceBytes = texBin.pixelData.data() + record.dataOffset;
        std::memcpy(mapped + transferOffset, sourceBytes, static_cast<std::size_t>(record.dataUncompressedSize));
        transferOffset += record.dataUncompressedSize;
    }
    SDL_UnmapGPUTransferBuffer(m_device, transferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (commandBuffer == nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
        SDL_ReleaseGPUTexture(m_device, texture);
        throwSdlError(("Failed to acquire command buffer for foliage imposter " + std::string(label) + " upload.").c_str());
    }

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    if (copyPass == nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
        SDL_ReleaseGPUTexture(m_device, texture);
        throwSdlError(("Failed to begin foliage imposter " + std::string(label) + " upload copy pass.").c_str());
    }

    for (std::size_t textureSlot = 0; textureSlot < textureIndices.size(); ++textureSlot)
    {
        const RuntimeAssets::TextureRecord& record = texBin.textures[textureIndices[textureSlot]];
        std::uint64_t sourceOffset = textureOffsets[textureSlot];
        for (std::uint32_t layerIndex = 0; layerIndex < record.layerCount; ++layerIndex)
        {
            for (std::uint32_t mipIndex = 0; mipIndex < record.mipCount; ++mipIndex)
            {
                const std::uint32_t mipWidth = RuntimeAssets::TextureMipExtent(record.width, mipIndex);
                const std::uint32_t mipHeight = RuntimeAssets::TextureMipExtent(record.height, mipIndex);
                const std::uint64_t mipByteSize = RuntimeAssets::CalculateTextureMipByteSize(runtimeFormat, mipWidth, mipHeight);

                SDL_GPUTextureTransferInfo source{};
                source.transfer_buffer = transferBuffer;
                source.offset = static_cast<Uint32>(sourceOffset);
                source.pixels_per_row = textureTransferStrideExtent(runtimeFormat, mipWidth);
                source.rows_per_layer = textureTransferStrideExtent(runtimeFormat, mipHeight);

                SDL_GPUTextureRegion destination{};
                destination.texture = texture;
                destination.mip_level = mipIndex;
                destination.layer = static_cast<Uint32>(textureSlot * record.layerCount + layerIndex);
                destination.w = mipWidth;
                destination.h = mipHeight;
                destination.d = 1u;
                SDL_UploadToGPUTexture(copyPass, &source, &destination, false);
                sourceOffset += mipByteSize;
            }
        }
    }

    SDL_EndGPUCopyPass(copyPass);
    const bool submitted = SDL_SubmitGPUCommandBuffer(commandBuffer);
    SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
    if (!submitted)
    {
        SDL_ReleaseGPUTexture(m_device, texture);
        throwSdlError(("Failed to submit foliage imposter " + std::string(label) + " texture upload.").c_str());
    }

    return texture;
}

void FoliageImposterRenderer::createPipeline(const std::filesystem::path& shaderDirectory)
{
    SDL_GPUShader* vertexShader = createShader(
        shaderDirectory / "foliage_imposter.vert.spv",
        SDL_GPU_SHADERSTAGE_VERTEX,
        1,
        4);
    SDL_GPUShader* fragmentShader = createShader(
        shaderDirectory / "foliage_imposter.frag.spv",
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        1,
        0,
        2);

    SDL_GPUVertexBufferDescription vertexBufferDescriptions[1]{};
    vertexBufferDescriptions[0].slot = 0;
    vertexBufferDescriptions[0].pitch = sizeof(Vertex);
    vertexBufferDescriptions[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute vertexAttributes[2]{};
    vertexAttributes[0].location = 0;
    vertexAttributes[0].buffer_slot = 0;
    vertexAttributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    vertexAttributes[0].offset = offsetof(Vertex, corner);
    vertexAttributes[1].location = 1;
    vertexAttributes[1].buffer_slot = 0;
    vertexAttributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    vertexAttributes[1].offset = offsetof(Vertex, uv0);

    SDL_GPUColorTargetBlendState blendState{};
    blendState.enable_blend = false;
    blendState.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    blendState.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blendState.color_blend_op = SDL_GPU_BLENDOP_ADD;
    blendState.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blendState.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blendState.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    blendState.color_write_mask =
        SDL_GPU_COLORCOMPONENT_R |
        SDL_GPU_COLORCOMPONENT_G |
        SDL_GPU_COLORCOMPONENT_B |
        SDL_GPU_COLORCOMPONENT_A;

    SDL_GPUColorTargetDescription colorTargetDescription{};
    colorTargetDescription.format = m_colorFormat;
    colorTargetDescription.blend_state = blendState;

    SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.vertex_shader = vertexShader;
    pipelineInfo.fragment_shader = fragmentShader;
    pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipelineInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipelineInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipelineInfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipelineInfo.rasterizer_state.enable_depth_clip = true;
    pipelineInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pipelineInfo.depth_stencil_state.enable_depth_test = true;
    pipelineInfo.depth_stencil_state.enable_depth_write = true;
    pipelineInfo.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    pipelineInfo.target_info.num_color_targets = 1;
    pipelineInfo.target_info.color_target_descriptions = &colorTargetDescription;
    pipelineInfo.target_info.has_depth_stencil_target = true;
    pipelineInfo.target_info.depth_stencil_format = m_depthFormat;
    pipelineInfo.vertex_input_state.num_vertex_buffers = 1;
    pipelineInfo.vertex_input_state.vertex_buffer_descriptions = vertexBufferDescriptions;
    pipelineInfo.vertex_input_state.num_vertex_attributes = 2;
    pipelineInfo.vertex_input_state.vertex_attributes = vertexAttributes;

    m_pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
    SDL_ReleaseGPUShader(m_device, fragmentShader);
    SDL_ReleaseGPUShader(m_device, vertexShader);
    if (m_pipeline == nullptr)
    {
        throwSdlError("Failed to create foliage impostor pipeline.");
    }
}

void FoliageImposterRenderer::createMaterialSampler()
{
    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.min_lod = 0.0f;
    samplerInfo.max_lod = 16.0f;
    m_materialSampler = SDL_CreateGPUSampler(m_device, &samplerInfo);
    if (m_materialSampler == nullptr)
    {
        throwSdlError("Failed to create foliage imposter sampler.");
    }
}

void FoliageImposterRenderer::createPagePoolBuffers()
{
    SDL_GPUBufferCreateInfo pagePoolBufferInfo{};
    pagePoolBufferInfo.usage =
        SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ |
        SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
        SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    pagePoolBufferInfo.size = FoliageConfig::kPagePoolCapacity * kPageByteSize;
    m_pagePoolBuffer = SDL_CreateGPUBuffer(m_device, &pagePoolBufferInfo);
    if (m_pagePoolBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage page-pool GPU buffer.");
    }
}

void FoliageImposterRenderer::createIndirectAndDrawMetadataBuffers()
{
    SDL_GPUBufferCreateInfo metadataBufferInfo{};
    metadataBufferInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    metadataBufferInfo.size = static_cast<Uint32>(sizeof(DrawMetadataGpu) * m_drawMetadata.size());
    m_drawMetadataBuffer = SDL_CreateGPUBuffer(m_device, &metadataBufferInfo);
    if (m_drawMetadataBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage draw-metadata buffer.");
    }

    SDL_GPUTransferBufferCreateInfo metadataTransferInfo{};
    metadataTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    metadataTransferInfo.size = metadataBufferInfo.size;
    m_drawMetadataTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &metadataTransferInfo);
    if (m_drawMetadataTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage draw-metadata transfer buffer.");
    }

    SDL_GPUBufferCreateInfo indirectBufferInfo{};
    indirectBufferInfo.usage = SDL_GPU_BUFFERUSAGE_INDIRECT;
    indirectBufferInfo.size = static_cast<Uint32>(sizeof(SDL_GPUIndexedIndirectDrawCommand) * m_drawCommands.size());
    m_indirectBuffer = SDL_CreateGPUBuffer(m_device, &indirectBufferInfo);
    if (m_indirectBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage indirect buffer.");
    }

    SDL_GPUTransferBufferCreateInfo indirectTransferInfo{};
    indirectTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    indirectTransferInfo.size = indirectBufferInfo.size;
    m_indirectTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &indirectTransferInfo);
    if (m_indirectTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage indirect transfer buffer.");
    }
}

void FoliageImposterRenderer::createClassMetadataBuffer()
{
    SDL_GPUBufferCreateInfo classBufferInfo{};
    classBufferInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    classBufferInfo.size = static_cast<Uint32>(sizeof(TreeClassGpu) * m_treeClassesGpu.size());
    m_treeClassBuffer = SDL_CreateGPUBuffer(m_device, &classBufferInfo);
    if (m_treeClassBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage imposter class-metadata buffer.");
    }

    SDL_GPUTransferBufferCreateInfo classTransferInfo{};
    classTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    classTransferInfo.size = classBufferInfo.size;
    m_treeClassTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &classTransferInfo);
    if (m_treeClassTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage imposter class transfer buffer.");
    }

    void* mappedClasses = SDL_MapGPUTransferBuffer(m_device, m_treeClassTransferBuffer, true);
    std::memcpy(mappedClasses, m_treeClassesGpu.data(), classBufferInfo.size);
    SDL_UnmapGPUTransferBuffer(m_device, m_treeClassTransferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (commandBuffer == nullptr)
    {
        throwSdlError("Failed to acquire command buffer for foliage imposter class upload.");
    }

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    if (copyPass == nullptr)
    {
        throwSdlError("Failed to begin foliage imposter class copy pass.");
    }

    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = m_treeClassTransferBuffer;
    SDL_GPUBufferRegion destination{};
    destination.buffer = m_treeClassBuffer;
    destination.size = classBufferInfo.size;
    SDL_UploadToGPUBuffer(copyPass, &source, &destination, true);
    SDL_EndGPUCopyPass(copyPass);

    if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
    {
        throwSdlError("Failed to submit foliage imposter class upload.");
    }
}

void FoliageImposterRenderer::createQuadBuffers()
{
    const std::array<Vertex, 4> vertices{
        Vertex{ glm::vec2(-1.0f, 0.0f), glm::vec2(0.0f, 1.0f) },
        Vertex{ glm::vec2( 1.0f, 0.0f), glm::vec2(1.0f, 1.0f) },
        Vertex{ glm::vec2(-1.0f, 1.0f), glm::vec2(0.0f, 0.0f) },
        Vertex{ glm::vec2( 1.0f, 1.0f), glm::vec2(1.0f, 0.0f) },
    };
    const std::array<std::uint16_t, 6> indices{ 0u, 1u, 2u, 2u, 1u, 3u };

    SDL_GPUBufferCreateInfo vertexBufferInfo{};
    vertexBufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertexBufferInfo.size = static_cast<Uint32>(sizeof(Vertex) * vertices.size());
    m_quadVertexBuffer = SDL_CreateGPUBuffer(m_device, &vertexBufferInfo);
    if (m_quadVertexBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage imposter quad vertex buffer.");
    }

    SDL_GPUBufferCreateInfo indexBufferInfo{};
    indexBufferInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    indexBufferInfo.size = static_cast<Uint32>(sizeof(std::uint16_t) * indices.size());
    m_quadIndexBuffer = SDL_CreateGPUBuffer(m_device, &indexBufferInfo);
    if (m_quadIndexBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage imposter quad index buffer.");
    }

    SDL_GPUTransferBufferCreateInfo vertexTransferInfo{};
    vertexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    vertexTransferInfo.size = vertexBufferInfo.size;
    m_quadVertexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &vertexTransferInfo);
    if (m_quadVertexTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage imposter quad vertex transfer buffer.");
    }

    SDL_GPUTransferBufferCreateInfo indexTransferInfo{};
    indexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    indexTransferInfo.size = indexBufferInfo.size;
    m_quadIndexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &indexTransferInfo);
    if (m_quadIndexTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage imposter quad index transfer buffer.");
    }

    void* mappedVertices = SDL_MapGPUTransferBuffer(m_device, m_quadVertexTransferBuffer, true);
    std::memcpy(mappedVertices, vertices.data(), sizeof(Vertex) * vertices.size());
    SDL_UnmapGPUTransferBuffer(m_device, m_quadVertexTransferBuffer);

    void* mappedIndices = SDL_MapGPUTransferBuffer(m_device, m_quadIndexTransferBuffer, true);
    std::memcpy(mappedIndices, indices.data(), sizeof(std::uint16_t) * indices.size());
    SDL_UnmapGPUTransferBuffer(m_device, m_quadIndexTransferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (commandBuffer == nullptr)
    {
        throwSdlError("Failed to acquire command buffer for foliage imposter quad upload.");
    }

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    if (copyPass == nullptr)
    {
        throwSdlError("Failed to begin foliage imposter quad copy pass.");
    }

    SDL_GPUTransferBufferLocation vertexSource{};
    vertexSource.transfer_buffer = m_quadVertexTransferBuffer;
    SDL_GPUBufferRegion vertexDestination{};
    vertexDestination.buffer = m_quadVertexBuffer;
    vertexDestination.size = vertexBufferInfo.size;
    SDL_UploadToGPUBuffer(copyPass, &vertexSource, &vertexDestination, true);

    SDL_GPUTransferBufferLocation indexSource{};
    indexSource.transfer_buffer = m_quadIndexTransferBuffer;
    SDL_GPUBufferRegion indexDestination{};
    indexDestination.buffer = m_quadIndexBuffer;
    indexDestination.size = indexBufferInfo.size;
    SDL_UploadToGPUBuffer(copyPass, &indexSource, &indexDestination, true);
    SDL_EndGPUCopyPass(copyPass);

    if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
    {
        throwSdlError("Failed to submit foliage imposter quad upload.");
    }
}
