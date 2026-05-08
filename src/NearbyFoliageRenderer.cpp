#include "NearbyFoliageRenderer.hpp"

#include "AppConfig.hpp"
#include "LightingSystem.hpp"
#include "PerformanceCapture.hpp"
#include "SkyboxRenderer.hpp"
#include "assets/RuntimeAssetReader.hpp"

#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <unordered_map>

namespace
{
constexpr std::uint32_t kDecodeComputeThreadCountX = 64u;
constexpr std::uint32_t kDecodeComputeThreadCountY = 1u;
constexpr std::uint32_t kDecodeComputeThreadCountZ = 1u;
constexpr float kNearbyLod0MaxDistanceMeters = 25.0f;
constexpr float kNearbyLod1MaxDistanceMeters = 50.0f;
constexpr float kNearbyLod2MaxDistanceMeters = 100.0f;

constexpr std::uint32_t kDecodedNearbyMeshIdMask = 0x0000FFFFu;
constexpr std::uint32_t kDecodedNearbyFlagsShift = 16u;

std::uint32_t decodedNearbyMeshId(std::uint32_t packedMeta)
{
    return packedMeta & kDecodedNearbyMeshIdMask;
}

std::uint32_t decodedNearbyFlags(std::uint32_t packedMeta)
{
    return packedMeta >> kDecodedNearbyFlagsShift;
}

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

SDL_GPUTextureFormat textureFormatFromRuntimeFormat(RuntimeAssets::TextureFormat format)
{
    switch (format)
    {
    case RuntimeAssets::TextureFormat::RGBA8_SRGB:
        return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    case RuntimeAssets::TextureFormat::RGBA8_UNORM:
    default:
        return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    }
}

std::filesystem::path executableRelativePath(const std::filesystem::path& relativePath)
{
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr)
    {
        throw std::runtime_error(std::string("Failed to resolve executable base path: ") + SDL_GetError());
    }

    const std::filesystem::path resolvedPath = std::filesystem::path(basePath) / relativePath;
    return resolvedPath;
}
}

void NearbyFoliageRenderer::initialize(
    SDL_GPUDevice* device,
    SDL_GPUTextureFormat colorFormat,
    SDL_GPUTextureFormat depthFormat,
    const std::filesystem::path& shaderDirectory)
{
    initializeRendererBase(device, colorFormat, depthFormat);
    createDecodeBuffers();
    createMaterialSampler();
    createDefaultTextures();
    loadRuntimeAssets();
    createDrawBuffers();
    createPipeline(shaderDirectory);
    createDecodeComputePipeline(shaderDirectory);
    resetTransientState();
}

void NearbyFoliageRenderer::shutdown()
{
    if (m_pipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
        m_pipeline = nullptr;
    }
    if (m_decodeComputePipeline != nullptr)
    {
        SDL_ReleaseGPUComputePipeline(m_device, m_decodeComputePipeline);
        m_decodeComputePipeline = nullptr;
    }
    if (m_baseColorTextureArray != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_baseColorTextureArray);
        m_baseColorTextureArray = nullptr;
    }
    if (m_normalTextureArray != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_normalTextureArray);
        m_normalTextureArray = nullptr;
    }
    if (m_roughnessTextureArray != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_roughnessTextureArray);
        m_roughnessTextureArray = nullptr;
    }
    if (m_specularTextureArray != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_specularTextureArray);
        m_specularTextureArray = nullptr;
    }
    if (m_aoTextureArray != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_aoTextureArray);
        m_aoTextureArray = nullptr;
    }
    if (m_subsurfaceTextureArray != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_subsurfaceTextureArray);
        m_subsurfaceTextureArray = nullptr;
    }
    m_loadedMaterials.clear();
    m_materialGpuRecords.clear();
    m_drawMetadataGpu.clear();
    m_drawCommands.clear();
    for (auto& classLods : m_loadedClassLods)
    {
        for (LoadedLodAsset& lod : classLods)
        {
            lod.name.clear();
            lod.drawParts.clear();
        }
    }
    if (m_materialSampler != nullptr)
    {
        SDL_ReleaseGPUSampler(m_device, m_materialSampler);
        m_materialSampler = nullptr;
    }
    if (m_materialTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_materialTransferBuffer);
        m_materialTransferBuffer = nullptr;
    }
    if (m_materialBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_materialBuffer);
        m_materialBuffer = nullptr;
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
    if (m_drawInstanceTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_drawInstanceTransferBuffer);
        m_drawInstanceTransferBuffer = nullptr;
    }
    if (m_drawInstanceBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_drawInstanceBuffer);
        m_drawInstanceBuffer = nullptr;
    }
    for (PendingReadback& readback : m_pendingReadbacks)
    {
        readback.fence.reset();
        if (readback.transferBuffer != nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(m_device, readback.transferBuffer);
            readback.transferBuffer = nullptr;
        }
        readback.entryIndex = FoliageConfig::kNearbyDecodedPageLruCapacity;
    }
    if (m_decodedZeroTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_decodedZeroTransferBuffer);
        m_decodedZeroTransferBuffer = nullptr;
    }
    if (m_decodedOutputBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_decodedOutputBuffer);
        m_decodedOutputBuffer = nullptr;
    }
    if (m_decodeRequestTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_decodeRequestTransferBuffer);
        m_decodeRequestTransferBuffer = nullptr;
    }
    if (m_decodeRequestBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_decodeRequestBuffer);
        m_decodeRequestBuffer = nullptr;
    }
    if (m_meshIndexTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_meshIndexTransferBuffer);
        m_meshIndexTransferBuffer = nullptr;
    }
    if (m_meshIndexBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_meshIndexBuffer);
        m_meshIndexBuffer = nullptr;
    }
    if (m_meshVertexTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_meshVertexTransferBuffer);
        m_meshVertexTransferBuffer = nullptr;
    }
    if (m_meshVertexBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_meshVertexBuffer);
        m_meshVertexBuffer = nullptr;
    }

    resetTransientState();
    m_decodedPages.fill({});
    m_activeTreeClassCount = 0u;
    m_runtimeAssetsLoaded = false;
}

void NearbyFoliageRenderer::setActiveCamera(const Position& cameraPosition)
{
    setActiveCameraPosition(cameraPosition);
}

void NearbyFoliageRenderer::beginFrame(std::uint64_t frameIndex)
{
    m_frameIndex = frameIndex;
    resetTransientState();
}

void NearbyFoliageRenderer::collectCompletedDecodedPages()
{
    HELLO_PROFILE_SCOPE("NearbyFoliageRenderer::CollectCompletedDecodedPages");

    for (PendingReadback& readback : m_pendingReadbacks)
    {
        if (!readback.fence || !readback.fence->isSignaled())
        {
            continue;
        }

        if (readback.entryIndex < m_decodedPages.size())
        {
            DecodedPageEntry& entry = m_decodedPages[readback.entryIndex];
            if (entry.readbackPending &&
                entry.key == readback.key &&
                entry.contentVersion == readback.contentVersion &&
                entry.layoutVersion == FoliageConfig::kNearbyDecodedInstanceLayoutVersion)
            {
                const void* mappedInstances = SDL_MapGPUTransferBuffer(m_device, readback.transferBuffer, false);
                std::memcpy(entry.instances.data(), mappedInstances, kDecodedPageByteSize);
                SDL_UnmapGPUTransferBuffer(m_device, readback.transferBuffer);
                entry.valid = true;
                entry.readbackPending = false;
                entry.liveCount = readback.liveCount;
            }
            else
            {
                SDL_UnmapGPUTransferBuffer(m_device, readback.transferBuffer);
            }
        }

        readback.fence.reset();
        readback.entryIndex = FoliageConfig::kNearbyDecodedPageLruCapacity;
        readback.key = {};
        readback.contentVersion = 0u;
        readback.liveCount = 0u;
    }
}

bool NearbyFoliageRenderer::makeResident(
    const WorldGridQuadtreeLeafId& pageKey,
    const FoliageReadyPageInfo& sourcePageInfo,
    std::uint64_t frameIndex)
{
    addTopologyHint(pageKey);

    std::uint16_t entryIndex = findEntryIndex(pageKey);
    if (entryIndex != FoliageConfig::kNearbyDecodedPageLruCapacity)
    {
        DecodedPageEntry& entry = m_decodedPages[entryIndex];
        entry.lastUsedFrame = frameIndex;
        if (entryMatchesSource(entry, pageKey, sourcePageInfo))
        {
            return entry.valid && !entry.readbackPending;
        }

        if (entry.readbackPending || m_pendingDecodeCount >= m_pendingDecodeRequests.size())
        {
            return false;
        }

        entry = {
            .key = pageKey,
            .sourcePageIndex = sourcePageInfo.pageIndex,
            .liveCount = sourcePageInfo.liveCount,
            .contentVersion = sourcePageInfo.contentVersion,
            .layoutVersion = FoliageConfig::kNearbyDecodedInstanceLayoutVersion,
            .valid = false,
            .readbackPending = true,
            .lastUsedFrame = frameIndex,
        };
        m_pendingDecodeRequests[m_pendingDecodeCount++] = {
            .entryIndex = entryIndex,
            .key = pageKey,
            .sourcePageInfo = sourcePageInfo,
        };
        return false;
    }

    if (m_pendingDecodeCount >= m_pendingDecodeRequests.size())
    {
        return false;
    }

    entryIndex = findReusableEntryIndex();
    if (entryIndex == FoliageConfig::kNearbyDecodedPageLruCapacity)
    {
        return false;
    }

    m_decodedPages[entryIndex] = {
        .key = pageKey,
        .sourcePageIndex = sourcePageInfo.pageIndex,
        .liveCount = sourcePageInfo.liveCount,
        .contentVersion = sourcePageInfo.contentVersion,
        .layoutVersion = FoliageConfig::kNearbyDecodedInstanceLayoutVersion,
        .valid = false,
        .readbackPending = true,
        .lastUsedFrame = frameIndex,
    };
    m_pendingDecodeRequests[m_pendingDecodeCount++] = {
        .entryIndex = entryIndex,
        .key = pageKey,
        .sourcePageInfo = sourcePageInfo,
    };
    return false;
}

void NearbyFoliageRenderer::addNearbyInstancesForPage(
    const WorldGridQuadtreeLeafId& pageKey,
    std::uint16_t terrainSliceIndex,
    const Position& nearCenter,
    float nearRadiusMeters)
{
    const std::uint16_t entryIndex = findEntryIndex(pageKey);
    if (entryIndex == FoliageConfig::kNearbyDecodedPageLruCapacity)
    {
        return;
    }
    if (m_activeTreeClassCount == 0u)
    {
        return;
    }

    const DecodedPageEntry& entry = m_decodedPages[entryIndex];
    if (!entry.valid || entry.readbackPending)
    {
        return;
    }

    const auto [pageOrigin, pageMaxCorner] = worldGridQuadtreeLeafBounds(pageKey);
    (void)pageMaxCorner;
    const glm::dvec3 pageWorld = pageOrigin.worldPosition();
    const glm::dvec3 nearWorld = nearCenter.worldPosition();
    const float localNearX = static_cast<float>(nearWorld.x - pageWorld.x);
    const float localNearZ = static_cast<float>(nearWorld.z - pageWorld.z);
    const float rangePadding = nearRadiusMeters + FoliageConfig::kNearbyDecodeRangePaddingMeters;
    const float cellSize = static_cast<float>(FoliageConfig::kCandidateCellSizeMeters);
    const int minCandidateX = std::max(
        0,
        static_cast<int>(std::floor((localNearX - rangePadding) / cellSize)));
    const int maxCandidateX = std::min(
        static_cast<int>(FoliageConfig::kCandidateGridResolution - 1u),
        static_cast<int>(std::floor((localNearX + rangePadding) / cellSize)));
    const int minCandidateZ = std::max(
        0,
        static_cast<int>(std::floor((localNearZ - rangePadding) / cellSize)));
    const int maxCandidateZ = std::min(
        static_cast<int>(FoliageConfig::kCandidateGridResolution - 1u),
        static_cast<int>(std::floor((localNearZ + rangePadding) / cellSize)));

    if (minCandidateX > maxCandidateX || minCandidateZ > maxCandidateZ)
    {
        return;
    }

    const glm::vec3 localPageOrigin = localPositionFromWorldPosition(pageOrigin);
    const float radiusSquared = nearRadiusMeters * nearRadiusMeters;
    for (int candidateZ = minCandidateZ; candidateZ <= maxCandidateZ; ++candidateZ)
    {
        for (int candidateX = minCandidateX; candidateX <= maxCandidateX; ++candidateX)
        {
            const std::uint32_t candidateSlot =
                static_cast<std::uint32_t>((candidateZ * static_cast<int>(FoliageConfig::kCandidateGridResolution)) + candidateX);
            const DecodedNearbyFoliageInstance& instance = entry.instances[candidateSlot];
            if ((decodedNearbyFlags(instance.packedMeta) & NearbyFoliageInstance_Resident) == 0u)
            {
                continue;
            }

            const float deltaX = instance.localX - localNearX;
            const float deltaZ = instance.localZ - localNearZ;
            const float distanceSquared = (deltaX * deltaX) + (deltaZ * deltaZ);
            if (distanceSquared > radiusSquared)
            {
                continue;
            }

            if (m_drawCount >= m_drawInstances.size())
            {
                return;
            }

            const std::uint32_t meshClass = std::min(decodedNearbyMeshId(instance.packedMeta), m_activeTreeClassCount - 1u);
            const std::uint32_t lodIndex =
                distanceSquared <= (kNearbyLod0MaxDistanceMeters * kNearbyLod0MaxDistanceMeters) ? 0u :
                (distanceSquared <= (kNearbyLod1MaxDistanceMeters * kNearbyLod1MaxDistanceMeters) ? 1u : 2u);

            m_drawInstances[m_drawCount++] = {
                .pageOriginAndSlice = glm::vec4(
                    localPageOrigin.x,
                    localPageOrigin.y,
                    localPageOrigin.z,
                    static_cast<float>(terrainSliceIndex)),
                .localOffsetAndMesh = glm::vec4(
                    instance.localX,
                    instance.localZ,
                    static_cast<float>(meshClass),
                    static_cast<float>(lodIndex)),
                .rotationAndReserved = glm::vec4(instance.rotationRadians, 0.0f, 0.0f, 0.0f),
            };
        }
    }
}

void NearbyFoliageRenderer::upload(SDL_GPUCopyPass* copyPass)
{
    HELLO_PROFILE_SCOPE("NearbyFoliageRenderer::Upload");

    if (m_drawCount == 0 || !m_runtimeAssetsLoaded || m_drawCommands.empty())
    {
        m_activeDrawCommandCount = 0u;
        return;
    }

    std::uint32_t writeIndex = 0u;
    for (std::uint32_t treeClass = 0; treeClass < m_activeTreeClassCount; ++treeClass)
    {
        for (std::uint32_t lodIndex = 0; lodIndex < kNearbyLodCount; ++lodIndex)
        {
            const std::uint32_t groupIndex = (treeClass * kNearbyLodCount) + lodIndex;
            m_groupFirstInstances[groupIndex] = writeIndex;
            for (std::uint32_t drawIndex = 0; drawIndex < m_drawCount; ++drawIndex)
            {
                const std::uint32_t instanceClass = std::min(
                    static_cast<std::uint32_t>(m_drawInstances[drawIndex].localOffsetAndMesh.z + 0.5f),
                    m_activeTreeClassCount - 1u);
                const std::uint32_t instanceLod = std::min(
                    static_cast<std::uint32_t>(m_drawInstances[drawIndex].localOffsetAndMesh.w + 0.5f),
                    kNearbyLodCount - 1u);
                if (instanceClass != treeClass || instanceLod != lodIndex)
                {
                    continue;
                }

                m_groupedDrawInstances[writeIndex++] = m_drawInstances[drawIndex];
            }
            m_groupInstanceCounts[groupIndex] = writeIndex - m_groupFirstInstances[groupIndex];
        }
    }

    void* mappedInstances = SDL_MapGPUTransferBuffer(m_device, m_drawInstanceTransferBuffer, true);
    std::memcpy(mappedInstances, m_groupedDrawInstances.data(), sizeof(DrawInstanceGpu) * m_drawCount);
    SDL_UnmapGPUTransferBuffer(m_device, m_drawInstanceTransferBuffer);

    SDL_GPUTransferBufferLocation instanceSource{};
    instanceSource.transfer_buffer = m_drawInstanceTransferBuffer;
    SDL_GPUBufferRegion instanceDestination{};
    instanceDestination.buffer = m_drawInstanceBuffer;
    instanceDestination.size = static_cast<Uint32>(sizeof(DrawInstanceGpu) * m_drawCount);
    SDL_UploadToGPUBuffer(copyPass, &instanceSource, &instanceDestination, true);

    m_activeDrawCommandCount = 0u;
    for (std::uint32_t treeClass = 0; treeClass < m_activeTreeClassCount; ++treeClass)
    {
        for (std::uint32_t lodIndex = 0; lodIndex < kNearbyLodCount; ++lodIndex)
        {
            const std::uint32_t groupIndex = (treeClass * kNearbyLodCount) + lodIndex;
            if (m_groupInstanceCounts[groupIndex] == 0u)
            {
                continue;
            }

            for (const LoadedDrawPart& drawPart : m_loadedClassLods[treeClass][lodIndex].drawParts)
            {
                m_drawMetadataGpu[m_activeDrawCommandCount].instanceOffsetAndMaterial = glm::uvec4(
                    m_groupFirstInstances[groupIndex],
                    drawPart.materialIndex,
                    0u,
                    0u);
                m_drawCommands[m_activeDrawCommandCount] = makeDrawCommand(
                    drawPart.indexCount,
                    m_groupInstanceCounts[groupIndex],
                    drawPart.firstIndex,
                    drawPart.vertexOffset);
                ++m_activeDrawCommandCount;
            }
        }
    }

    void* mappedMetadata = SDL_MapGPUTransferBuffer(m_device, m_drawMetadataTransferBuffer, true);
    std::memcpy(
        mappedMetadata,
        m_drawMetadataGpu.data(),
        sizeof(DrawMetadataGpu) * m_activeDrawCommandCount);
    SDL_UnmapGPUTransferBuffer(m_device, m_drawMetadataTransferBuffer);

    SDL_GPUTransferBufferLocation metadataSource{};
    metadataSource.transfer_buffer = m_drawMetadataTransferBuffer;
    SDL_GPUBufferRegion metadataDestination{};
    metadataDestination.buffer = m_drawMetadataBuffer;
    metadataDestination.size = static_cast<Uint32>(sizeof(DrawMetadataGpu) * m_activeDrawCommandCount);
    SDL_UploadToGPUBuffer(copyPass, &metadataSource, &metadataDestination, true);

    void* mappedIndirect = SDL_MapGPUTransferBuffer(m_device, m_indirectTransferBuffer, true);
    std::memcpy(
        mappedIndirect,
        m_drawCommands.data(),
        sizeof(SDL_GPUIndexedIndirectDrawCommand) * m_activeDrawCommandCount);
    SDL_UnmapGPUTransferBuffer(m_device, m_indirectTransferBuffer);

    SDL_GPUTransferBufferLocation indirectSource{};
    indirectSource.transfer_buffer = m_indirectTransferBuffer;
    SDL_GPUBufferRegion indirectDestination{};
    indirectDestination.buffer = m_indirectBuffer;
    indirectDestination.size = static_cast<Uint32>(sizeof(SDL_GPUIndexedIndirectDrawCommand) * m_activeDrawCommandCount);
    SDL_UploadToGPUBuffer(copyPass, &indirectSource, &indirectDestination, true);
}

void NearbyFoliageRenderer::dispatchDecodedPageExpansions(
    SDL_GPUCommandBuffer* commandBuffer,
    SDL_GPUBuffer* sourcePagePoolBuffer)
{
    HELLO_PROFILE_SCOPE("NearbyFoliageRenderer::DispatchDecodedPageExpansions");

    if (sourcePagePoolBuffer == nullptr || m_pendingDecodeCount == 0 || m_lastDispatchedDecodeCount != 0)
    {
        return;
    }

    for (std::uint16_t requestIndex = 0; requestIndex < m_pendingDecodeCount; ++requestIndex)
    {
        const PendingDecodeRequest& request = m_pendingDecodeRequests[requestIndex];
        m_decodeRequestsGpu[requestIndex] = {
            .sourcePageData = glm::uvec4(
                request.sourcePageInfo.pageIndex,
                request.sourcePageInfo.liveCount,
                request.sourcePageInfo.seed,
                requestIndex),
        };
        m_lastDispatchedDecodeRequests[requestIndex] = request;
    }

    void* mappedRequests = SDL_MapGPUTransferBuffer(m_device, m_decodeRequestTransferBuffer, true);
    std::memcpy(mappedRequests, m_decodeRequestsGpu.data(), sizeof(DecodeRequestGpu) * m_pendingDecodeCount);
    SDL_UnmapGPUTransferBuffer(m_device, m_decodeRequestTransferBuffer);

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    if (copyPass == nullptr)
    {
        throwSdlError("Failed to begin nearby foliage decode upload copy pass.");
    }

    SDL_GPUTransferBufferLocation requestSource{};
    requestSource.transfer_buffer = m_decodeRequestTransferBuffer;
    SDL_GPUBufferRegion requestDestination{};
    requestDestination.buffer = m_decodeRequestBuffer;
    requestDestination.size = static_cast<Uint32>(sizeof(DecodeRequestGpu) * m_pendingDecodeCount);
    SDL_UploadToGPUBuffer(copyPass, &requestSource, &requestDestination, false);

    SDL_GPUTransferBufferLocation zeroSource{};
    zeroSource.transfer_buffer = m_decodedZeroTransferBuffer;
    SDL_GPUBufferRegion zeroDestination{};
    zeroDestination.buffer = m_decodedOutputBuffer;
    zeroDestination.size = static_cast<Uint32>(kDecodedPageByteSize * m_pendingDecodeCount);
    SDL_UploadToGPUBuffer(copyPass, &zeroSource, &zeroDestination, false);

    SDL_EndGPUCopyPass(copyPass);

    SDL_GPUStorageBufferReadWriteBinding decodedOutputBinding{};
    decodedOutputBinding.buffer = m_decodedOutputBuffer;
    decodedOutputBinding.cycle = false;

    SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(commandBuffer, nullptr, 0, &decodedOutputBinding, 1);
    if (computePass == nullptr)
    {
        throwSdlError("Failed to begin nearby foliage decode compute pass.");
    }

    SDL_BindGPUComputePipeline(computePass, m_decodeComputePipeline);
    SDL_GPUBuffer* readonlyStorageBuffers[]{
        m_decodeRequestBuffer,
        sourcePagePoolBuffer,
    };
    SDL_BindGPUComputeStorageBuffers(computePass, 0, readonlyStorageBuffers, 2);

    const std::uint32_t groupCountX =
        (FoliageConfig::kCandidateSlotCount + kDecodeComputeThreadCountX - 1u) / kDecodeComputeThreadCountX;
    SDL_DispatchGPUCompute(computePass, groupCountX, 1u, m_pendingDecodeCount);
    SDL_EndGPUComputePass(computePass);

    m_lastDispatchedDecodeCount = m_pendingDecodeCount;
    m_pendingDecodeCount = 0u;
}

void NearbyFoliageRenderer::queueDecodedPageDownloads(SDL_GPUCopyPass* copyPass)
{
    HELLO_PROFILE_SCOPE("NearbyFoliageRenderer::QueueDecodedPageDownloads");

    m_pendingFenceReadbackCount = 0u;
    if (m_lastDispatchedDecodeCount == 0u)
    {
        return;
    }

    for (std::uint16_t requestIndex = 0; requestIndex < m_lastDispatchedDecodeCount; ++requestIndex)
    {
        std::uint16_t readbackSlotIndex = FoliageConfig::kNearbyReadbackSlotCount;
        for (std::uint16_t slotIndex = 0; slotIndex < m_pendingReadbacks.size(); ++slotIndex)
        {
            if (!m_pendingReadbacks[slotIndex].fence &&
                m_pendingReadbacks[slotIndex].entryIndex == FoliageConfig::kNearbyDecodedPageLruCapacity)
            {
                readbackSlotIndex = slotIndex;
                break;
            }
        }

        if (readbackSlotIndex == FoliageConfig::kNearbyReadbackSlotCount)
        {
            continue;
        }

        PendingReadback& readback = m_pendingReadbacks[readbackSlotIndex];
        SDL_GPUBufferRegion source{};
        source.buffer = m_decodedOutputBuffer;
        source.offset = static_cast<Uint32>(requestIndex * kDecodedPageByteSize);
        source.size = static_cast<Uint32>(kDecodedPageByteSize);

        SDL_GPUTransferBufferLocation destination{};
        destination.transfer_buffer = readback.transferBuffer;
        destination.offset = 0u;
        SDL_DownloadFromGPUBuffer(copyPass, &source, &destination);

        readback.entryIndex = m_lastDispatchedDecodeRequests[requestIndex].entryIndex;
        readback.key = m_lastDispatchedDecodeRequests[requestIndex].key;
        readback.contentVersion = m_lastDispatchedDecodeRequests[requestIndex].sourcePageInfo.contentVersion;
        readback.liveCount = m_lastDispatchedDecodeRequests[requestIndex].sourcePageInfo.liveCount;
        m_pendingFenceReadbackSlots[m_pendingFenceReadbackCount++] = readbackSlotIndex;
    }

    m_lastDispatchedDecodeCount = 0u;
}

void NearbyFoliageRenderer::attachSubmittedFence(const std::shared_ptr<SubmittedGpuFence>& fence)
{
    for (std::uint16_t fenceSlotIndex = 0; fenceSlotIndex < m_pendingFenceReadbackCount; ++fenceSlotIndex)
    {
        PendingReadback& readback = m_pendingReadbacks[m_pendingFenceReadbackSlots[fenceSlotIndex]];
        readback.fence = fence;
    }
    m_pendingFenceReadbackCount = 0u;
}

void NearbyFoliageRenderer::render(
    SDL_GPURenderPass* renderPass,
    SDL_GPUCommandBuffer* commandBuffer,
    const glm::mat4& viewProjection,
    const LightingSystem& lightingSystem,
    const SkyboxRenderer& skyboxRenderer,
    SDL_GPUBuffer* terrainHeightmapBuffer) const
{
    HELLO_PROFILE_SCOPE("NearbyFoliageRenderer::Render");

    if (m_drawCount == 0 ||
        terrainHeightmapBuffer == nullptr ||
        !m_runtimeAssetsLoaded ||
        m_meshVertexBuffer == nullptr ||
        m_meshIndexBuffer == nullptr ||
        m_materialSampler == nullptr ||
        m_activeDrawCommandCount == 0u ||
        m_baseColorTextureArray == nullptr ||
        m_normalTextureArray == nullptr ||
        m_roughnessTextureArray == nullptr ||
        m_specularTextureArray == nullptr ||
        m_aoTextureArray == nullptr ||
        m_subsurfaceTextureArray == nullptr ||
        skyboxRenderer.cubemapTexture() == nullptr ||
        skyboxRenderer.atmosphereLutTexture() == nullptr ||
        skyboxRenderer.cubemapSampler() == nullptr ||
        skyboxRenderer.atmosphereSampler() == nullptr ||
        m_drawMetadataBuffer == nullptr ||
        m_materialBuffer == nullptr)
    {
        return;
    }

    VertexUniforms vertexUniforms{};
    vertexUniforms.viewProjection = viewProjection;
    SDL_PushGPUVertexUniformData(commandBuffer, 0, &vertexUniforms, sizeof(vertexUniforms));

    SDL_BindGPUGraphicsPipeline(renderPass, m_pipeline);

    const SDL_GPUBufferBinding vertexBinding{ m_meshVertexBuffer, 0 };
    SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);
    const SDL_GPUBufferBinding indexBinding{ m_meshIndexBuffer, 0 };
    SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    SDL_GPUBuffer* vertexStorageBuffers[]{
        m_drawInstanceBuffer,
        terrainHeightmapBuffer,
        m_drawMetadataBuffer,
    };
    SDL_BindGPUVertexStorageBuffers(renderPass, 0, vertexStorageBuffers, 3);

    SDL_GPUTextureSamplerBinding samplerBindings[8]{
        { m_baseColorTextureArray, m_materialSampler },
        { m_normalTextureArray, m_materialSampler },
        { m_roughnessTextureArray, m_materialSampler },
        { m_specularTextureArray, m_materialSampler },
        { m_aoTextureArray, m_materialSampler },
        { m_subsurfaceTextureArray, m_materialSampler },
        { skyboxRenderer.cubemapTexture(), skyboxRenderer.cubemapSampler() },
        { skyboxRenderer.atmosphereLutTexture(), skyboxRenderer.atmosphereSampler() },
    };
    SDL_BindGPUFragmentSamplers(renderPass, 0, samplerBindings, 8);

    SDL_GPUBuffer* fragmentStorageBuffers[]{
        m_materialBuffer,
    };
    SDL_BindGPUFragmentStorageBuffers(renderPass, 0, fragmentStorageBuffers, 1);

    const glm::vec3 sunDirection = lightingSystem.sunDirection();
    FragmentUniforms fragmentUniforms{};
    fragmentUniforms.sunDirectionIntensity = glm::vec4(sunDirection, lightingSystem.sun().intensity);
    fragmentUniforms.sunColorAmbient = glm::vec4(lightingSystem.sun().color, AppConfig::Terrain::kAmbientLight);
    fragmentUniforms.shadingParams0 = glm::vec4(0.04f, 1.0f, 0.45f, 0.0f);
    const SkyboxRenderer::SharedSkyUniforms sharedSkyUniforms =
        skyboxRenderer.buildSharedSkyUniforms(m_activeCameraPosition.localPosition().y, lightingSystem);
    fragmentUniforms.skyRotation = sharedSkyUniforms.skyRotation;
    fragmentUniforms.atmosphereParams = sharedSkyUniforms.atmosphereParams;
    fragmentUniforms.sunDirectionTimeOfDay = sharedSkyUniforms.sunDirectionTimeOfDay;
    SDL_PushGPUFragmentUniformData(commandBuffer, 0, &fragmentUniforms, sizeof(fragmentUniforms));

    SDL_DrawGPUIndexedPrimitivesIndirect(renderPass, m_indirectBuffer, 0, m_activeDrawCommandCount);
}

std::uint32_t NearbyFoliageRenderer::drawCallCount() const
{
    return m_activeDrawCommandCount;
}

std::uint32_t NearbyFoliageRenderer::decodedResidentCount() const
{
    std::uint32_t count = 0u;
    for (const DecodedPageEntry& entry : m_decodedPages)
    {
        if (entry.valid && !entry.readbackPending)
        {
            ++count;
        }
    }
    return count;
}

std::uint32_t NearbyFoliageRenderer::decodedPendingCount() const
{
    std::uint32_t count = 0u;
    for (const DecodedPageEntry& entry : m_decodedPages)
    {
        if (entry.readbackPending)
        {
            ++count;
        }
    }
    return count;
}

SDL_GPUIndexedIndirectDrawCommand NearbyFoliageRenderer::makeDrawCommand(
    std::uint32_t indexCount,
    std::uint32_t instanceCount,
    std::uint32_t firstIndex,
    std::int32_t vertexOffset)
{
    SDL_GPUIndexedIndirectDrawCommand command{};
    command.num_indices = indexCount;
    command.num_instances = instanceCount;
    command.first_index = firstIndex;
    command.vertex_offset = vertexOffset;
    command.first_instance = 0u;
    return command;
}

void NearbyFoliageRenderer::createPipeline(const std::filesystem::path& shaderDirectory)
{
    SDL_GPUShader* vertexShader = createShader(
        shaderDirectory / "nearby_foliage.vert.spv",
        SDL_GPU_SHADERSTAGE_VERTEX,
        1,
        3);
    SDL_GPUShader* fragmentShader = createShader(
        shaderDirectory / "nearby_foliage.frag.spv",
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        1,
        1,
        8);

    SDL_GPUVertexBufferDescription vertexBufferDescription{};
    vertexBufferDescription.slot = 0;
    vertexBufferDescription.pitch = sizeof(Vertex);
    vertexBufferDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute vertexAttributes[4]{};
    vertexAttributes[0].location = 0;
    vertexAttributes[0].buffer_slot = 0;
    vertexAttributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertexAttributes[0].offset = offsetof(Vertex, position);
    vertexAttributes[1].location = 1;
    vertexAttributes[1].buffer_slot = 0;
    vertexAttributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertexAttributes[1].offset = offsetof(Vertex, normal);
    vertexAttributes[2].location = 2;
    vertexAttributes[2].buffer_slot = 0;
    vertexAttributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    vertexAttributes[2].offset = offsetof(Vertex, tangent);
    vertexAttributes[3].location = 3;
    vertexAttributes[3].buffer_slot = 0;
    vertexAttributes[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    vertexAttributes[3].offset = offsetof(Vertex, uv0);

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
    pipelineInfo.vertex_input_state.vertex_buffer_descriptions = &vertexBufferDescription;
    pipelineInfo.vertex_input_state.num_vertex_attributes = 4;
    pipelineInfo.vertex_input_state.vertex_attributes = vertexAttributes;

    m_pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
    SDL_ReleaseGPUShader(m_device, fragmentShader);
    SDL_ReleaseGPUShader(m_device, vertexShader);
    if (m_pipeline == nullptr)
    {
        throwSdlError("Failed to create nearby foliage mesh pipeline.");
    }
}

void NearbyFoliageRenderer::createDecodeComputePipeline(const std::filesystem::path& shaderDirectory)
{
    const std::vector<std::uint8_t> bytes = readShaderCode(shaderDirectory / "nearby_foliage_decode.comp.spv");

    SDL_GPUComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.code_size = bytes.size();
    pipelineInfo.code = bytes.data();
    pipelineInfo.entrypoint = "main";
    pipelineInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    pipelineInfo.num_samplers = 0;
    pipelineInfo.num_readonly_storage_textures = 0;
    pipelineInfo.num_readonly_storage_buffers = 2;
    pipelineInfo.num_readwrite_storage_textures = 0;
    pipelineInfo.num_readwrite_storage_buffers = 1;
    pipelineInfo.num_uniform_buffers = 0;
    pipelineInfo.threadcount_x = kDecodeComputeThreadCountX;
    pipelineInfo.threadcount_y = kDecodeComputeThreadCountY;
    pipelineInfo.threadcount_z = kDecodeComputeThreadCountZ;

    m_decodeComputePipeline = SDL_CreateGPUComputePipeline(m_device, &pipelineInfo);
    if (m_decodeComputePipeline == nullptr)
    {
        throwSdlError("Failed to create nearby foliage decode compute pipeline.");
    }
}

void NearbyFoliageRenderer::createMaterialSampler()
{
    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.min_lod = 0.0f;
    samplerInfo.max_lod = 0.0f;
    m_materialSampler = SDL_CreateGPUSampler(m_device, &samplerInfo);
    if (m_materialSampler == nullptr)
    {
        throwSdlError("Failed to create nearby foliage material sampler.");
    }
}

SDL_GPUTexture* NearbyFoliageRenderer::createTexture2d(
    SDL_GPUTextureFormat format,
    std::uint32_t width,
    std::uint32_t height,
    const std::byte* bytes,
    std::size_t byteCount) const
{
    SDL_GPUTextureCreateInfo textureInfo{};
    textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
    textureInfo.format = format;
    textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    textureInfo.width = width;
    textureInfo.height = height;
    textureInfo.layer_count_or_depth = 1;
    textureInfo.num_levels = 1;
    textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(m_device, &textureInfo);
    if (texture == nullptr)
    {
        throwSdlError("Failed to create nearby foliage material texture.");
    }

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<Uint32>(byteCount);
    SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
    if (transferBuffer == nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, texture);
        throwSdlError("Failed to create nearby foliage texture upload buffer.");
    }

    void* mapped = SDL_MapGPUTransferBuffer(m_device, transferBuffer, false);
    std::memcpy(mapped, bytes, byteCount);
    SDL_UnmapGPUTransferBuffer(m_device, transferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (commandBuffer == nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
        SDL_ReleaseGPUTexture(m_device, texture);
        throwSdlError("Failed to acquire command buffer for nearby foliage texture upload.");
    }

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    if (copyPass == nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
        SDL_ReleaseGPUTexture(m_device, texture);
        throwSdlError("Failed to begin nearby foliage texture upload copy pass.");
    }

    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = transferBuffer;
    source.offset = 0u;
    source.pixels_per_row = width;
    source.rows_per_layer = height;

    SDL_GPUTextureRegion destination{};
    destination.texture = texture;
    destination.w = width;
    destination.h = height;
    destination.d = 1;
    SDL_UploadToGPUTexture(copyPass, &source, &destination, true);
    SDL_EndGPUCopyPass(copyPass);

    if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
    {
        SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
        SDL_ReleaseGPUTexture(m_device, texture);
        throwSdlError("Failed to submit nearby foliage texture upload.");
    }

    SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
    return texture;
}

void NearbyFoliageRenderer::createDefaultTextures()
{
}

void NearbyFoliageRenderer::createMeshBuffers(
    std::span<const Vertex> vertices,
    std::span<const std::uint32_t> indices)
{
    SDL_GPUBufferCreateInfo vertexBufferInfo{};
    vertexBufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertexBufferInfo.size = static_cast<Uint32>(vertices.size_bytes());
    m_meshVertexBuffer = SDL_CreateGPUBuffer(m_device, &vertexBufferInfo);
    if (m_meshVertexBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage mesh vertex buffer.");
    }

    SDL_GPUBufferCreateInfo indexBufferInfo{};
    indexBufferInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    indexBufferInfo.size = static_cast<Uint32>(indices.size_bytes());
    m_meshIndexBuffer = SDL_CreateGPUBuffer(m_device, &indexBufferInfo);
    if (m_meshIndexBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage mesh index buffer.");
    }

    SDL_GPUTransferBufferCreateInfo vertexTransferInfo{};
    vertexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    vertexTransferInfo.size = vertexBufferInfo.size;
    m_meshVertexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &vertexTransferInfo);
    if (m_meshVertexTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage mesh vertex transfer buffer.");
    }

    SDL_GPUTransferBufferCreateInfo indexTransferInfo{};
    indexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    indexTransferInfo.size = indexBufferInfo.size;
    m_meshIndexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &indexTransferInfo);
    if (m_meshIndexTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage mesh index transfer buffer.");
    }

    void* mappedVertices = SDL_MapGPUTransferBuffer(m_device, m_meshVertexTransferBuffer, true);
    std::memcpy(mappedVertices, vertices.data(), vertices.size_bytes());
    SDL_UnmapGPUTransferBuffer(m_device, m_meshVertexTransferBuffer);

    void* mappedIndices = SDL_MapGPUTransferBuffer(m_device, m_meshIndexTransferBuffer, true);
    std::memcpy(mappedIndices, indices.data(), indices.size_bytes());
    SDL_UnmapGPUTransferBuffer(m_device, m_meshIndexTransferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (commandBuffer == nullptr)
    {
        throwSdlError("Failed to acquire command buffer for nearby foliage mesh upload.");
    }

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    if (copyPass == nullptr)
    {
        throwSdlError("Failed to begin nearby foliage mesh copy pass.");
    }

    SDL_GPUTransferBufferLocation vertexSource{};
    vertexSource.transfer_buffer = m_meshVertexTransferBuffer;
    SDL_GPUBufferRegion vertexDestination{};
    vertexDestination.buffer = m_meshVertexBuffer;
    vertexDestination.size = vertexBufferInfo.size;
    SDL_UploadToGPUBuffer(copyPass, &vertexSource, &vertexDestination, false);

    SDL_GPUTransferBufferLocation indexSource{};
    indexSource.transfer_buffer = m_meshIndexTransferBuffer;
    SDL_GPUBufferRegion indexDestination{};
    indexDestination.buffer = m_meshIndexBuffer;
    indexDestination.size = indexBufferInfo.size;
    SDL_UploadToGPUBuffer(copyPass, &indexSource, &indexDestination, false);
    SDL_EndGPUCopyPass(copyPass);

    if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
    {
        throwSdlError("Failed to submit nearby foliage mesh upload.");
    }
}

std::vector<std::byte> NearbyFoliageRenderer::resampleTextureRgba(
    const std::byte* sourcePixels,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight) const
{
    std::vector<std::byte> result(static_cast<std::size_t>(targetWidth) * targetHeight * 4u);
    for (std::uint32_t y = 0; y < targetHeight; ++y)
    {
        const std::uint32_t sourceY = sourceHeight == targetHeight
            ? y
            : std::min(sourceHeight - 1u, static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * sourceHeight) / targetHeight));
        for (std::uint32_t x = 0; x < targetWidth; ++x)
        {
            const std::uint32_t sourceX = sourceWidth == targetWidth
                ? x
                : std::min(sourceWidth - 1u, static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * sourceWidth) / targetWidth));
            const std::size_t sourceIndex = (static_cast<std::size_t>(sourceY) * sourceWidth + sourceX) * 4u;
            const std::size_t targetIndex = (static_cast<std::size_t>(y) * targetWidth + x) * 4u;
            std::memcpy(result.data() + targetIndex, sourcePixels + sourceIndex, 4u);
        }
    }
    return result;
}

void NearbyFoliageRenderer::createMaterialResources(
    const RuntimeAssets::LoadedTexBinView& texBin,
    const RuntimeAssets::LoadedAssetBinView& assetBin,
    const std::unordered_map<std::uint32_t, std::uint32_t>& usedBaseColorTextures,
    const std::unordered_map<std::uint32_t, std::uint32_t>& usedNormalTextures,
    const std::unordered_map<std::uint32_t, std::uint32_t>& usedRoughnessTextures,
    const std::unordered_map<std::uint32_t, std::uint32_t>& usedSpecularTextures,
    const std::unordered_map<std::uint32_t, std::uint32_t>& usedAoTextures,
    const std::unordered_map<std::uint32_t, std::uint32_t>& usedSubsurfaceTextures)
{
    auto findMaxExtent = [&](const std::unordered_map<std::uint32_t, std::uint32_t>& textureToLayer) {
        glm::uvec2 extent(1u, 1u);
        for (const auto& [textureIndex, layerIndex] : textureToLayer)
        {
            if (textureIndex == std::numeric_limits<std::uint32_t>::max())
            {
                continue;
            }
            const RuntimeAssets::TextureRecord& record = texBin.textures[textureIndex];
            extent.x = std::max(extent.x, record.width);
            extent.y = std::max(extent.y, record.height);
            (void)layerIndex;
        }
        return extent;
    };

    auto createArray = [&](SDL_GPUTextureFormat format, const glm::uvec2& extent, std::uint32_t layers) -> SDL_GPUTexture* {
        SDL_GPUTextureCreateInfo textureInfo{};
        textureInfo.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
        textureInfo.format = format;
        textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        textureInfo.width = extent.x;
        textureInfo.height = extent.y;
        textureInfo.layer_count_or_depth = layers;
        textureInfo.num_levels = 1;
        textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
        SDL_GPUTexture* texture = SDL_CreateGPUTexture(m_device, &textureInfo);
        if (texture == nullptr)
        {
            throwSdlError("Failed to create nearby foliage texture array.");
        }
        return texture;
    };

    auto uploadArray = [&](SDL_GPUTexture* texture,
        const glm::uvec2& extent,
        const std::unordered_map<std::uint32_t, std::uint32_t>& textureToLayer,
        const std::array<std::byte, 4>& defaultPixel) {
        const std::size_t layerSize = static_cast<std::size_t>(extent.x) * extent.y * 4u;
        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = static_cast<Uint32>(layerSize * textureToLayer.size());
        SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
        if (transferBuffer == nullptr)
        {
            throwSdlError("Failed to create nearby foliage texture-array upload buffer.");
        }

        std::byte* mapped = static_cast<std::byte*>(SDL_MapGPUTransferBuffer(m_device, transferBuffer, false));
        for (const auto& [textureIndex, layerIndex] : textureToLayer)
        {
            std::vector<std::byte> layerPixels;
            if (textureIndex == std::numeric_limits<std::uint32_t>::max())
            {
                layerPixels.assign(layerSize, std::byte{});
                for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(extent.x) * extent.y; ++pixel)
                {
                    std::memcpy(layerPixels.data() + (pixel * 4u), defaultPixel.data(), 4u);
                }
            }
            else
            {
                const RuntimeAssets::TextureRecord& record = texBin.textures[textureIndex];
                const std::byte* sourcePixels = texBin.pixelData.data() + record.dataOffset;
                layerPixels = resampleTextureRgba(sourcePixels, record.width, record.height, extent.x, extent.y);
            }

            std::memcpy(mapped + (static_cast<std::size_t>(layerIndex) * layerSize), layerPixels.data(), layerSize);
        }
        SDL_UnmapGPUTransferBuffer(m_device, transferBuffer);

        SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
        if (commandBuffer == nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
            throwSdlError("Failed to acquire command buffer for nearby foliage texture array upload.");
        }

        SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
        if (copyPass == nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
            throwSdlError("Failed to begin nearby foliage texture array copy pass.");
        }

        for (const auto& [textureIndex, layerIndex] : textureToLayer)
        {
            SDL_GPUTextureTransferInfo source{};
            source.transfer_buffer = transferBuffer;
            source.offset = static_cast<Uint32>(static_cast<std::size_t>(layerIndex) * layerSize);
            source.pixels_per_row = extent.x;
            source.rows_per_layer = extent.y;

            SDL_GPUTextureRegion destination{};
            destination.texture = texture;
            destination.layer = layerIndex;
            destination.w = extent.x;
            destination.h = extent.y;
            destination.d = 1;
            SDL_UploadToGPUTexture(copyPass, &source, &destination, false);
            (void)textureIndex;
        }

        SDL_EndGPUCopyPass(copyPass);
        if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
        {
            SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
            throwSdlError("Failed to submit nearby foliage texture array upload.");
        }
        SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
    };

    const glm::uvec2 baseColorExtent = findMaxExtent(usedBaseColorTextures);
    const glm::uvec2 normalExtent = findMaxExtent(usedNormalTextures);
    const glm::uvec2 roughnessExtent = findMaxExtent(usedRoughnessTextures);
    const glm::uvec2 specularExtent = findMaxExtent(usedSpecularTextures);
    const glm::uvec2 aoExtent = findMaxExtent(usedAoTextures);
    const glm::uvec2 subsurfaceExtent = findMaxExtent(usedSubsurfaceTextures);

    m_baseColorTextureArray = createArray(
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB,
        baseColorExtent,
        static_cast<std::uint32_t>(usedBaseColorTextures.size()));
    m_normalTextureArray = createArray(
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        normalExtent,
        static_cast<std::uint32_t>(usedNormalTextures.size()));
    m_roughnessTextureArray = createArray(
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        roughnessExtent,
        static_cast<std::uint32_t>(usedRoughnessTextures.size()));
    m_specularTextureArray = createArray(
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        specularExtent,
        static_cast<std::uint32_t>(usedSpecularTextures.size()));
    m_aoTextureArray = createArray(
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        aoExtent,
        static_cast<std::uint32_t>(usedAoTextures.size()));
    m_subsurfaceTextureArray = createArray(
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        subsurfaceExtent,
        static_cast<std::uint32_t>(usedSubsurfaceTextures.size()));

    uploadArray(
        m_baseColorTextureArray,
        baseColorExtent,
        usedBaseColorTextures,
        { std::byte{ 0xFF }, std::byte{ 0xFF }, std::byte{ 0xFF }, std::byte{ 0xFF } });
    uploadArray(
        m_normalTextureArray,
        normalExtent,
        usedNormalTextures,
        { std::byte{ 0x80 }, std::byte{ 0x80 }, std::byte{ 0xFF }, std::byte{ 0xFF } });
    uploadArray(
        m_roughnessTextureArray,
        roughnessExtent,
        usedRoughnessTextures,
        { std::byte{ 0xFF }, std::byte{ 0xFF }, std::byte{ 0xFF }, std::byte{ 0xFF } });
    uploadArray(
        m_specularTextureArray,
        specularExtent,
        usedSpecularTextures,
        { std::byte{ 0x0A }, std::byte{ 0x0A }, std::byte{ 0x0A }, std::byte{ 0xFF } });
    uploadArray(
        m_aoTextureArray,
        aoExtent,
        usedAoTextures,
        { std::byte{ 0xFF }, std::byte{ 0xFF }, std::byte{ 0xFF }, std::byte{ 0xFF } });
    uploadArray(
        m_subsurfaceTextureArray,
        subsurfaceExtent,
        usedSubsurfaceTextures,
        { std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0xFF } });

    m_materialGpuRecords.assign(assetBin.materials.size(), {});
    for (std::uint32_t materialIndex = 0; materialIndex < assetBin.materials.size(); ++materialIndex)
    {
        const RuntimeAssets::MaterialRecord& material = assetBin.materials[materialIndex];
        const std::uint32_t baseLayer = usedBaseColorTextures.contains(material.baseColorTextureIndex)
            ? usedBaseColorTextures.at(material.baseColorTextureIndex)
            : 0u;
        const std::uint32_t normalLayer = usedNormalTextures.contains(material.normalTextureIndex)
            ? usedNormalTextures.at(material.normalTextureIndex)
            : 0u;
        const std::uint32_t roughnessLayer = usedRoughnessTextures.contains(material.roughnessTextureIndex)
            ? usedRoughnessTextures.at(material.roughnessTextureIndex)
            : 0u;
        const std::uint32_t specularLayer = usedSpecularTextures.contains(material.specularTextureIndex)
            ? usedSpecularTextures.at(material.specularTextureIndex)
            : 0u;
        const std::uint32_t aoLayer = usedAoTextures.contains(material.aoTextureIndex)
            ? usedAoTextures.at(material.aoTextureIndex)
            : 0u;
        const std::uint32_t subsurfaceLayer = usedSubsurfaceTextures.contains(material.subsurfaceTextureIndex)
            ? usedSubsurfaceTextures.at(material.subsurfaceTextureIndex)
            : 0u;
        m_materialGpuRecords[materialIndex].layers0 = glm::uvec4(
            baseLayer,
            normalLayer,
            roughnessLayer,
            specularLayer);
        m_materialGpuRecords[materialIndex].layers1 = glm::uvec4(
            aoLayer,
            subsurfaceLayer,
            material.flags,
            0u);
        m_materialGpuRecords[materialIndex].params = glm::vec4(material.alphaCutoff, 0.0f, 0.0f, 0.0f);
    }

    SDL_GPUBufferCreateInfo materialBufferInfo{};
    materialBufferInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    materialBufferInfo.size = static_cast<Uint32>(sizeof(MaterialGpu) * m_materialGpuRecords.size());
    m_materialBuffer = SDL_CreateGPUBuffer(m_device, &materialBufferInfo);
    if (m_materialBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage material buffer.");
    }

    SDL_GPUTransferBufferCreateInfo materialTransferInfo{};
    materialTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    materialTransferInfo.size = materialBufferInfo.size;
    m_materialTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &materialTransferInfo);
    if (m_materialTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage material transfer buffer.");
    }

    void* mappedMaterials = SDL_MapGPUTransferBuffer(m_device, m_materialTransferBuffer, true);
    std::memcpy(mappedMaterials, m_materialGpuRecords.data(), materialBufferInfo.size);
    SDL_UnmapGPUTransferBuffer(m_device, m_materialTransferBuffer);

    SDL_GPUCommandBuffer* materialCommandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (materialCommandBuffer == nullptr)
    {
        throwSdlError("Failed to acquire command buffer for nearby foliage material upload.");
    }
    SDL_GPUCopyPass* materialCopyPass = SDL_BeginGPUCopyPass(materialCommandBuffer);
    if (materialCopyPass == nullptr)
    {
        throwSdlError("Failed to begin nearby foliage material upload copy pass.");
    }
    SDL_GPUTransferBufferLocation materialSource{};
    materialSource.transfer_buffer = m_materialTransferBuffer;
    SDL_GPUBufferRegion materialDestination{};
    materialDestination.buffer = m_materialBuffer;
    materialDestination.size = materialBufferInfo.size;
    SDL_UploadToGPUBuffer(materialCopyPass, &materialSource, &materialDestination, true);
    SDL_EndGPUCopyPass(materialCopyPass);
    if (!SDL_SubmitGPUCommandBuffer(materialCommandBuffer))
    {
        throwSdlError("Failed to submit nearby foliage material upload.");
    }
}

void NearbyFoliageRenderer::loadRuntimeAssets()
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
        throw std::runtime_error("Failed to load nearby foliage runtime assets: " + error);
    }

    std::vector<Vertex> meshVertices;
    meshVertices.reserve(meshBin.vertices.size());
    for (const RuntimeAssets::MeshVertex& sourceVertex : meshBin.vertices)
    {
        Vertex vertex{};
        vertex.position = glm::vec3(sourceVertex.position[0], sourceVertex.position[1], sourceVertex.position[2]);
        vertex.normal = glm::vec3(sourceVertex.normal[0], sourceVertex.normal[1], sourceVertex.normal[2]);
        vertex.tangent = glm::vec4(
            sourceVertex.tangent[0],
            sourceVertex.tangent[1],
            sourceVertex.tangent[2],
            sourceVertex.tangent[3]);
        vertex.uv0 = glm::vec2(sourceVertex.uv0[0], sourceVertex.uv0[1]);
        meshVertices.push_back(vertex);
    }
    createMeshBuffers(meshVertices, meshBin.indices);

    m_loadedMaterials.assign(assetBin.materials.size(), {});
    std::unordered_map<std::uint32_t, std::uint32_t> usedBaseColorTextures;
    std::unordered_map<std::uint32_t, std::uint32_t> usedNormalTextures;
    std::unordered_map<std::uint32_t, std::uint32_t> usedRoughnessTextures;
    std::unordered_map<std::uint32_t, std::uint32_t> usedSpecularTextures;
    std::unordered_map<std::uint32_t, std::uint32_t> usedAoTextures;
    std::unordered_map<std::uint32_t, std::uint32_t> usedSubsurfaceTextures;
    usedBaseColorTextures.emplace(std::numeric_limits<std::uint32_t>::max(), 0u);
    usedNormalTextures.emplace(std::numeric_limits<std::uint32_t>::max(), 0u);
    usedRoughnessTextures.emplace(std::numeric_limits<std::uint32_t>::max(), 0u);
    usedSpecularTextures.emplace(std::numeric_limits<std::uint32_t>::max(), 0u);
    usedAoTextures.emplace(std::numeric_limits<std::uint32_t>::max(), 0u);
    usedSubsurfaceTextures.emplace(std::numeric_limits<std::uint32_t>::max(), 0u);

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
        throw std::runtime_error("Nearby foliage runtime pack did not contain any pine assets.");
    }
    if (selectedAssetIndices.size() > kNearbyTreeClassCount)
    {
        throw std::runtime_error("Nearby foliage runtime pack contains more tree assets than the packed meshId budget allows.");
    }
    m_activeTreeClassCount = static_cast<std::uint32_t>(selectedAssetIndices.size());

    for (std::uint32_t treeClass = 0; treeClass < m_activeTreeClassCount; ++treeClass)
    {
        const RuntimeAssets::AssetRecord& assetRecord = assetBin.assets[selectedAssetIndices[treeClass]];
        const char* desiredAssetName = assetBin.stringAt(assetRecord.nameOffset);
        for (LoadedLodAsset& lod : m_loadedClassLods[treeClass])
        {
            lod.name = desiredAssetName;
            lod.drawParts.clear();
        }

        for (std::uint32_t meshRefOffset = 0; meshRefOffset < assetRecord.meshRefCount; ++meshRefOffset)
        {
            const RuntimeAssets::MeshRefRecord& meshRef = assetBin.meshRefs[assetRecord.firstMeshRef + meshRefOffset];
            if (meshRef.lodIndex >= kNearbyLodCount)
            {
                continue;
            }
            if (meshRef.meshIndex >= meshBin.meshes.size())
            {
                continue;
            }

            const RuntimeAssets::MeshRecord& mesh = meshBin.meshes[meshRef.meshIndex];
            LoadedLodAsset& lod = m_loadedClassLods[treeClass][meshRef.lodIndex];
            for (std::uint32_t submeshOffset = 0; submeshOffset < mesh.submeshCount; ++submeshOffset)
            {
                const RuntimeAssets::SubmeshRecord& submesh = meshBin.submeshes[mesh.firstSubmesh + submeshOffset];
                if (submesh.materialIndex >= assetBin.materials.size())
                {
                    continue;
                }

                const RuntimeAssets::MaterialRecord& material = assetBin.materials[submesh.materialIndex];
                const char* materialName = assetBin.stringAt(material.nameOffset);
                if (containsInsensitive(materialName, "billboard"))
                {
                    continue;
                }

                LoadedMaterialGpu& gpuMaterial = m_loadedMaterials[submesh.materialIndex];
                if (gpuMaterial.baseColorLayer == 0u &&
                    gpuMaterial.normalLayer == 0u &&
                    gpuMaterial.roughnessLayer == 0u &&
                    gpuMaterial.specularLayer == 0u &&
                    gpuMaterial.aoLayer == 0u &&
                    gpuMaterial.subsurfaceLayer == 0u &&
                    gpuMaterial.alphaCutoff == 0.0f &&
                    gpuMaterial.flags == 0u)
                {
                    if (!usedBaseColorTextures.contains(material.baseColorTextureIndex))
                    {
                        usedBaseColorTextures.emplace(
                            material.baseColorTextureIndex,
                            static_cast<std::uint32_t>(usedBaseColorTextures.size()));
                    }
                    if (!usedNormalTextures.contains(material.normalTextureIndex))
                    {
                        usedNormalTextures.emplace(
                            material.normalTextureIndex,
                            static_cast<std::uint32_t>(usedNormalTextures.size()));
                    }
                    if (!usedRoughnessTextures.contains(material.roughnessTextureIndex))
                    {
                        usedRoughnessTextures.emplace(
                            material.roughnessTextureIndex,
                            static_cast<std::uint32_t>(usedRoughnessTextures.size()));
                    }
                    if (!usedSpecularTextures.contains(material.specularTextureIndex))
                    {
                        usedSpecularTextures.emplace(
                            material.specularTextureIndex,
                            static_cast<std::uint32_t>(usedSpecularTextures.size()));
                    }
                    if (!usedAoTextures.contains(material.aoTextureIndex))
                    {
                        usedAoTextures.emplace(
                            material.aoTextureIndex,
                            static_cast<std::uint32_t>(usedAoTextures.size()));
                    }
                    if (!usedSubsurfaceTextures.contains(material.subsurfaceTextureIndex))
                    {
                        usedSubsurfaceTextures.emplace(
                            material.subsurfaceTextureIndex,
                            static_cast<std::uint32_t>(usedSubsurfaceTextures.size()));
                    }
                    gpuMaterial.baseColorLayer = usedBaseColorTextures.at(material.baseColorTextureIndex);
                    gpuMaterial.normalLayer = usedNormalTextures.at(material.normalTextureIndex);
                    gpuMaterial.roughnessLayer = usedRoughnessTextures.at(material.roughnessTextureIndex);
                    gpuMaterial.specularLayer = usedSpecularTextures.at(material.specularTextureIndex);
                    gpuMaterial.aoLayer = usedAoTextures.at(material.aoTextureIndex);
                    gpuMaterial.subsurfaceLayer = usedSubsurfaceTextures.at(material.subsurfaceTextureIndex);
                    gpuMaterial.alphaCutoff = material.alphaCutoff;
                    gpuMaterial.flags = material.flags;
                }

                lod.drawParts.push_back({
                    .indexCount = submesh.indexCount,
                    .firstIndex = submesh.firstIndex,
                    .vertexOffset = static_cast<std::int32_t>(submesh.firstVertex),
                    .materialIndex = submesh.materialIndex,
                });
            }
        }

        for (std::uint32_t lodIndex = 0; lodIndex < kNearbyLodCount; ++lodIndex)
        {
            if (m_loadedClassLods[treeClass][lodIndex].drawParts.empty())
            {
                throw std::runtime_error(
                    std::string("Nearby foliage selected asset is missing drawable LOD") +
                    std::to_string(lodIndex) + ": " + desiredAssetName);
            }
        }
    }

    createMaterialResources(
        texBin,
        assetBin,
        usedBaseColorTextures,
        usedNormalTextures,
        usedRoughnessTextures,
        usedSpecularTextures,
        usedAoTextures,
        usedSubsurfaceTextures);
    m_runtimeAssetsLoaded = true;
}

void NearbyFoliageRenderer::createDecodeBuffers()
{
    SDL_GPUBufferCreateInfo requestBufferInfo{};
    requestBufferInfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    requestBufferInfo.size = static_cast<Uint32>(sizeof(DecodeRequestGpu) * m_decodeRequestsGpu.size());
    m_decodeRequestBuffer = SDL_CreateGPUBuffer(m_device, &requestBufferInfo);
    if (m_decodeRequestBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage decode-request buffer.");
    }

    SDL_GPUTransferBufferCreateInfo requestTransferInfo{};
    requestTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    requestTransferInfo.size = requestBufferInfo.size;
    m_decodeRequestTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &requestTransferInfo);
    if (m_decodeRequestTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage decode-request transfer buffer.");
    }

    SDL_GPUBufferCreateInfo decodedOutputInfo{};
    decodedOutputInfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    decodedOutputInfo.size = static_cast<Uint32>(kDecodedPageByteSize * m_decodeRequestsGpu.size());
    m_decodedOutputBuffer = SDL_CreateGPUBuffer(m_device, &decodedOutputInfo);
    if (m_decodedOutputBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage decoded-output buffer.");
    }

    SDL_GPUTransferBufferCreateInfo zeroTransferInfo{};
    zeroTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    zeroTransferInfo.size = decodedOutputInfo.size;
    m_decodedZeroTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &zeroTransferInfo);
    if (m_decodedZeroTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage decoded-output zero transfer buffer.");
    }

    void* mappedZeroBytes = SDL_MapGPUTransferBuffer(m_device, m_decodedZeroTransferBuffer, true);
    std::memset(mappedZeroBytes, 0, zeroTransferInfo.size);
    SDL_UnmapGPUTransferBuffer(m_device, m_decodedZeroTransferBuffer);

    SDL_GPUTransferBufferCreateInfo readbackInfo{};
    readbackInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    readbackInfo.size = static_cast<Uint32>(kDecodedPageByteSize);
    for (PendingReadback& readback : m_pendingReadbacks)
    {
        readback.transferBuffer = SDL_CreateGPUTransferBuffer(m_device, &readbackInfo);
        if (readback.transferBuffer == nullptr)
        {
            throwSdlError("Failed to create nearby foliage decoded-page readback buffer.");
        }
        readback.entryIndex = FoliageConfig::kNearbyDecodedPageLruCapacity;
    }
}

void NearbyFoliageRenderer::createDrawBuffers()
{
    SDL_GPUBufferCreateInfo drawInstanceInfo{};
    drawInstanceInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    drawInstanceInfo.size = static_cast<Uint32>(sizeof(DrawInstanceGpu) * m_drawInstances.size());
    m_drawInstanceBuffer = SDL_CreateGPUBuffer(m_device, &drawInstanceInfo);
    if (m_drawInstanceBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage draw-instance buffer.");
    }

    SDL_GPUTransferBufferCreateInfo drawInstanceTransferInfo{};
    drawInstanceTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    drawInstanceTransferInfo.size = drawInstanceInfo.size;
    m_drawInstanceTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &drawInstanceTransferInfo);
    if (m_drawInstanceTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage draw-instance transfer buffer.");
    }

    SDL_GPUBufferCreateInfo indirectInfo{};
    indirectInfo.usage = SDL_GPU_BUFFERUSAGE_INDIRECT;
    const std::size_t maxDrawCount = [&]() {
        std::size_t total = 0;
        for (const auto& classLods : m_loadedClassLods)
        {
            for (const LoadedLodAsset& lod : classLods)
            {
                total += lod.drawParts.size();
            }
        }
        return total;
    }();
    m_drawCommands.resize(maxDrawCount);
    m_drawMetadataGpu.resize(maxDrawCount);
    indirectInfo.size = static_cast<Uint32>(sizeof(SDL_GPUIndexedIndirectDrawCommand) * maxDrawCount);
    m_indirectBuffer = SDL_CreateGPUBuffer(m_device, &indirectInfo);
    if (m_indirectBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage indirect buffer.");
    }

    SDL_GPUTransferBufferCreateInfo indirectTransferInfo{};
    indirectTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    indirectTransferInfo.size = indirectInfo.size;
    m_indirectTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &indirectTransferInfo);
    if (m_indirectTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage indirect transfer buffer.");
    }

    SDL_GPUBufferCreateInfo drawMetadataInfo{};
    drawMetadataInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    drawMetadataInfo.size = static_cast<Uint32>(sizeof(DrawMetadataGpu) * maxDrawCount);
    m_drawMetadataBuffer = SDL_CreateGPUBuffer(m_device, &drawMetadataInfo);
    if (m_drawMetadataBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage draw-metadata buffer.");
    }

    SDL_GPUTransferBufferCreateInfo drawMetadataTransferInfo{};
    drawMetadataTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    drawMetadataTransferInfo.size = drawMetadataInfo.size;
    m_drawMetadataTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &drawMetadataTransferInfo);
    if (m_drawMetadataTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage draw-metadata transfer buffer.");
    }
}

void NearbyFoliageRenderer::resetTransientState()
{
    m_pendingDecodeCount = 0u;
    m_lastDispatchedDecodeCount = 0u;
    m_pendingFenceReadbackCount = 0u;
    m_topologyHintCount = 0u;
    m_drawCount = 0u;
    m_activeDrawCommandCount = 0u;
    m_groupFirstInstances.fill(0u);
    m_groupInstanceCounts.fill(0u);
    m_pendingFenceReadbackSlots.fill(0u);
}

std::uint16_t NearbyFoliageRenderer::findEntryIndex(const WorldGridQuadtreeLeafId& pageKey) const
{
    for (std::uint16_t entryIndex = 0; entryIndex < m_decodedPages.size(); ++entryIndex)
    {
        const DecodedPageEntry& entry = m_decodedPages[entryIndex];
        if ((entry.valid || entry.readbackPending) && entry.key == pageKey)
        {
            return entryIndex;
        }
    }

    return FoliageConfig::kNearbyDecodedPageLruCapacity;
}

bool NearbyFoliageRenderer::entryMatchesSource(
    const DecodedPageEntry& entry,
    const WorldGridQuadtreeLeafId& pageKey,
    const FoliageReadyPageInfo& sourcePageInfo) const
{
    return
        entry.key == pageKey &&
        entry.sourcePageIndex == sourcePageInfo.pageIndex &&
        entry.contentVersion == sourcePageInfo.contentVersion &&
        entry.layoutVersion == FoliageConfig::kNearbyDecodedInstanceLayoutVersion;
}

bool NearbyFoliageRenderer::entryIsHintedThisFrame(const WorldGridQuadtreeLeafId& pageKey) const
{
    for (std::uint16_t hintIndex = 0; hintIndex < m_topologyHintCount; ++hintIndex)
    {
        if (m_topologyHints[hintIndex] == pageKey)
        {
            return true;
        }
    }

    return false;
}

void NearbyFoliageRenderer::addTopologyHint(const WorldGridQuadtreeLeafId& pageKey)
{
    if (entryIsHintedThisFrame(pageKey) || m_topologyHintCount >= m_topologyHints.size())
    {
        return;
    }

    m_topologyHints[m_topologyHintCount++] = pageKey;
}

std::uint16_t NearbyFoliageRenderer::findReusableEntryIndex() const
{
    std::uint16_t invalidEntryIndex = FoliageConfig::kNearbyDecodedPageLruCapacity;
    std::uint16_t oldestNonHintedEntryIndex = FoliageConfig::kNearbyDecodedPageLruCapacity;
    std::uint16_t oldestEntryIndex = FoliageConfig::kNearbyDecodedPageLruCapacity;
    std::uint64_t oldestNonHintedFrame = 0u;
    std::uint64_t oldestFrame = 0u;

    for (std::uint16_t entryIndex = 0; entryIndex < m_decodedPages.size(); ++entryIndex)
    {
        const DecodedPageEntry& entry = m_decodedPages[entryIndex];
        if (entry.readbackPending)
        {
            continue;
        }

        if (!entry.valid)
        {
            return entryIndex;
        }

        if (!entryIsHintedThisFrame(entry.key) &&
            (oldestNonHintedEntryIndex == FoliageConfig::kNearbyDecodedPageLruCapacity ||
             entry.lastUsedFrame < oldestNonHintedFrame))
        {
            oldestNonHintedEntryIndex = entryIndex;
            oldestNonHintedFrame = entry.lastUsedFrame;
        }

        if (oldestEntryIndex == FoliageConfig::kNearbyDecodedPageLruCapacity || entry.lastUsedFrame < oldestFrame)
        {
            oldestEntryIndex = entryIndex;
            oldestFrame = entry.lastUsedFrame;
        }

        if (invalidEntryIndex == FoliageConfig::kNearbyDecodedPageLruCapacity && !entry.valid)
        {
            invalidEntryIndex = entryIndex;
        }
    }

    if (oldestNonHintedEntryIndex != FoliageConfig::kNearbyDecodedPageLruCapacity)
    {
        return oldestNonHintedEntryIndex;
    }

    if (oldestEntryIndex != FoliageConfig::kNearbyDecodedPageLruCapacity)
    {
        return oldestEntryIndex;
    }

    return invalidEntryIndex;
}
