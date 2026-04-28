#include "QuadtreeMeshRenderer.hpp"

#include "AppConfig.hpp"
#include "PerformanceCapture.hpp"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
constexpr std::uint32_t kMainGridVertexResolution =
    AppConfig::Terrain::kHeightmapLeafResolution - (AppConfig::Terrain::kRenderedPatchInset * 2);
constexpr std::uint32_t kMainGridQuadResolution = kMainGridVertexResolution - 1;
constexpr float kRenderedLocalCoordOffset = static_cast<float>(AppConfig::Terrain::kRenderedPatchInset);
constexpr float kRenderedSampleCoordOffset = static_cast<float>(
    AppConfig::Terrain::kHeightmapLeafHalo + AppConfig::Terrain::kRenderedPatchInset);
constexpr std::size_t kHeightmapSliceFloatCount =
    static_cast<std::size_t>(AppConfig::Terrain::kHeightmapResolution) *
    static_cast<std::size_t>(AppConfig::Terrain::kHeightmapResolution);
constexpr std::size_t kHeightmapExtentsCount = AppConfig::Terrain::kHeightmapSliceCapacity;
constexpr std::uint32_t kHeightmapComputeThreadCountX = 16;
constexpr std::uint32_t kHeightmapComputeThreadCountY = 16;
constexpr std::uint32_t kHeightmapComputeThreadCountZ = 1;
constexpr std::int32_t kInitialMinHeightCentimeters = std::numeric_limits<std::int32_t>::max();
constexpr std::int32_t kInitialMaxHeightCentimeters = std::numeric_limits<std::int32_t>::lowest();
}

void QuadtreeMeshRenderer::initialize(
    SDL_GPUDevice* device,
    SDL_GPUTextureFormat colorFormat,
    SDL_GPUTextureFormat depthFormat,
    const std::filesystem::path& shaderDirectory
)
{
    initializeRendererBase(device, colorFormat, depthFormat);
    createStaticMeshResources();
    createPipeline(shaderDirectory);
    createHeightmapComputePipeline(shaderDirectory);

    SDL_GPUBufferCreateInfo instanceInfo{};
    instanceInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    instanceInfo.size = static_cast<Uint32>(sizeof(InstanceData) * m_instanceData.size());
    m_instanceBuffer = SDL_CreateGPUBuffer(m_device, &instanceInfo);
    if (m_instanceBuffer == nullptr)
    {
        throwSdlError("Failed to create quadtree mesh instance buffer.");
    }

    SDL_GPUTransferBufferCreateInfo instanceTransferInfo{};
    instanceTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    instanceTransferInfo.size = instanceInfo.size;
    m_instanceTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &instanceTransferInfo);
    if (m_instanceTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create quadtree mesh instance transfer buffer.");
    }

    SDL_GPUBufferCreateInfo indirectInfo{};
    indirectInfo.usage = SDL_GPU_BUFFERUSAGE_INDIRECT;
    indirectInfo.size = sizeof(SDL_GPUIndexedIndirectDrawCommand);
    m_indirectBuffer = SDL_CreateGPUBuffer(m_device, &indirectInfo);
    if (m_indirectBuffer == nullptr)
    {
        throwSdlError("Failed to create quadtree mesh indirect buffer.");
    }

    SDL_GPUTransferBufferCreateInfo indirectTransferInfo{};
    indirectTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    indirectTransferInfo.size = indirectInfo.size;
    m_indirectTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &indirectTransferInfo);
    if (m_indirectTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create quadtree mesh indirect transfer buffer.");
    }

    SDL_GPUBufferCreateInfo heightmapInfo{};
    heightmapInfo.usage =
        SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
        SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ |
        SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    heightmapInfo.size = static_cast<Uint32>(sizeof(float) * kHeightmapSliceFloatCount * AppConfig::Terrain::kHeightmapSliceCapacity);
    m_heightmapBuffer = SDL_CreateGPUBuffer(m_device, &heightmapInfo);
    if (m_heightmapBuffer == nullptr)
    {
        throwSdlError("Failed to create quadtree mesh heightmap buffer.");
    }

    SDL_GPUBufferCreateInfo heightmapGenerationInfo{};
    heightmapGenerationInfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    heightmapGenerationInfo.size = static_cast<Uint32>(sizeof(HeightmapGenerationUniforms) * m_pendingHeightmapGenerations.size());
    m_heightmapGenerationBuffer = SDL_CreateGPUBuffer(m_device, &heightmapGenerationInfo);
    if (m_heightmapGenerationBuffer == nullptr)
    {
        throwSdlError("Failed to create quadtree mesh generation buffer.");
    }

    SDL_GPUTransferBufferCreateInfo heightmapGenerationTransferInfo{};
    heightmapGenerationTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    heightmapGenerationTransferInfo.size = heightmapGenerationInfo.size;
    m_heightmapGenerationTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &heightmapGenerationTransferInfo);
    if (m_heightmapGenerationTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create quadtree mesh generation transfer buffer.");
    }

    SDL_GPUBufferCreateInfo extentsInfo{};
    extentsInfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    extentsInfo.size = static_cast<Uint32>(sizeof(GpuHeightmapExtents) * kHeightmapExtentsCount);
    m_heightmapExtentsBuffer = SDL_CreateGPUBuffer(m_device, &extentsInfo);
    if (m_heightmapExtentsBuffer == nullptr)
    {
        throwSdlError("Failed to create quadtree mesh heightmap extents buffer.");
    }

    SDL_GPUTransferBufferCreateInfo extentsInitTransferInfo{};
    extentsInitTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    extentsInitTransferInfo.size = extentsInfo.size;
    m_heightmapExtentsInitTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &extentsInitTransferInfo);
    if (m_heightmapExtentsInitTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create quadtree mesh extents init transfer buffer.");
    }

    SDL_GPUTransferBufferCreateInfo extentsDownloadInfo{};
    extentsDownloadInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    extentsDownloadInfo.size = extentsInfo.size;
    for (PendingExtentsReadback& readback : m_pendingExtentsReadbacks)
    {
        readback.transferBuffer = SDL_CreateGPUTransferBuffer(m_device, &extentsDownloadInfo);
        if (readback.transferBuffer == nullptr)
        {
            throwSdlError("Failed to create quadtree mesh extents download transfer buffer.");
        }
    }
}

void QuadtreeMeshRenderer::shutdown()
{
    for (PendingExtentsReadback& readback : m_pendingExtentsReadbacks)
    {
        if (readback.fence != nullptr)
        {
            SDL_ReleaseGPUFence(m_device, readback.fence);
            readback.fence = nullptr;
        }
        if (readback.transferBuffer != nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(m_device, readback.transferBuffer);
            readback.transferBuffer = nullptr;
        }
        readback.count = 0;
    }
    if (m_heightmapExtentsInitTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_heightmapExtentsInitTransferBuffer);
        m_heightmapExtentsInitTransferBuffer = nullptr;
    }
    if (m_heightmapExtentsBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_heightmapExtentsBuffer);
        m_heightmapExtentsBuffer = nullptr;
    }
    if (m_heightmapGenerationTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_heightmapGenerationTransferBuffer);
        m_heightmapGenerationTransferBuffer = nullptr;
    }
    if (m_heightmapGenerationBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_heightmapGenerationBuffer);
        m_heightmapGenerationBuffer = nullptr;
    }
    if (m_heightmapComputePipeline != nullptr)
    {
        SDL_ReleaseGPUComputePipeline(m_device, m_heightmapComputePipeline);
        m_heightmapComputePipeline = nullptr;
    }
    if (m_pipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
        m_pipeline = nullptr;
    }
    if (m_heightmapBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_heightmapBuffer);
        m_heightmapBuffer = nullptr;
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
    if (m_instanceTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_instanceTransferBuffer);
        m_instanceTransferBuffer = nullptr;
    }
    if (m_instanceBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_instanceBuffer);
        m_instanceBuffer = nullptr;
    }
    if (m_indexTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_indexTransferBuffer);
        m_indexTransferBuffer = nullptr;
    }
    if (m_indexBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_indexBuffer);
        m_indexBuffer = nullptr;
    }
    if (m_vertexTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_vertexTransferBuffer);
        m_vertexTransferBuffer = nullptr;
    }
    if (m_vertexBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_vertexBuffer);
        m_vertexBuffer = nullptr;
    }

    clear();
    m_pendingHeightmapGenerationCount = 0;
    m_lastDispatchedGenerationCount = 0;
    m_pendingFenceReadbackSlot = UINT16_MAX;
}

void QuadtreeMeshRenderer::clear()
{
    m_instanceCount = 0;
}

void QuadtreeMeshRenderer::setActiveCamera(const Position& cameraPosition)
{
    setActiveCameraPosition(cameraPosition);
}

void QuadtreeMeshRenderer::setTerrainHeightParams(float baseHeight, float heightAmplitude)
{
    m_terrainBaseHeight = baseHeight;
    m_terrainHeightAmplitude = heightAmplitude;
}

bool QuadtreeMeshRenderer::queueHeightmapGeneration(
    const WorldGridQuadtreeLeafId& leafId,
    std::uint16_t sliceIndex,
    const TerrainNoiseSettings& settings)
{
    if (m_pendingHeightmapGenerationCount >= m_pendingHeightmapGenerations.size())
    {
        return false;
    }

    const TerrainNoiseSettings sanitized = sanitizeTerrainNoiseSettings(settings);

    const auto [a, b] = worldGridQuadtreeLeafBounds(leafId);
    const double leafIntervalCount = static_cast<double>(AppConfig::Terrain::kHeightmapLeafIntervalCount);
    const double leafWorldMinX = a.worldPosition().x;
    const double leafWorldMinZ = a.worldPosition().z;
    const double leafWorldMaxX = b.worldPosition().x;
    const double leafWorldMaxZ = b.worldPosition().z;
    const double stepX = (leafWorldMaxX - leafWorldMinX) / leafIntervalCount;
    const double stepZ = (leafWorldMaxZ - leafWorldMinZ) / leafIntervalCount;
    const double worldMinX = leafWorldMinX - (stepX * static_cast<double>(AppConfig::Terrain::kHeightmapLeafHalo));
    const double worldMinZ = leafWorldMinZ - (stepZ * static_cast<double>(AppConfig::Terrain::kHeightmapLeafHalo));

    HeightmapGenerationUniforms& uniforms = m_pendingHeightmapGenerations[m_pendingHeightmapGenerationCount++];
    m_pendingGenerationLeafIds[m_pendingHeightmapGenerationCount - 1] = leafId;
    uniforms.sampleOriginAndStep = glm::vec4(
        static_cast<float>(worldMinX),
        static_cast<float>(worldMinZ),
        static_cast<float>(stepX),
        static_cast<float>(stepZ));
    uniforms.hillsLayerA = glm::vec4(
        static_cast<float>(sanitized.hills.wavelength),
        static_cast<float>(sanitized.hills.amplitude),
        static_cast<float>(sanitized.hills.bias),
        static_cast<float>(sanitized.hills.initialFrequency));
    uniforms.hillsLayerB = glm::vec4(
        static_cast<float>(sanitized.hills.initialAmplitude),
        static_cast<float>(sanitized.hills.octaveFrequencyScale),
        static_cast<float>(sanitized.hills.octaveAmplitudeScale),
        static_cast<float>(sanitized.hills.octaveCount));
    uniforms.hillsLayerC = glm::vec4(
        static_cast<float>(sanitized.hills.gradientDampenStrength),
        static_cast<float>(sanitized.hills.octaveRotationDegrees),
        0.0f,
        0.0f);
    uniforms.mediumLayerA = glm::vec4(
        static_cast<float>(sanitized.mediumDetail.wavelength),
        static_cast<float>(sanitized.mediumDetail.amplitude),
        static_cast<float>(sanitized.mediumDetail.bias),
        static_cast<float>(sanitized.mediumDetail.initialFrequency));
    uniforms.mediumLayerB = glm::vec4(
        static_cast<float>(sanitized.mediumDetail.initialAmplitude),
        static_cast<float>(sanitized.mediumDetail.octaveFrequencyScale),
        static_cast<float>(sanitized.mediumDetail.octaveAmplitudeScale),
        static_cast<float>(sanitized.mediumDetail.octaveCount));
    uniforms.mediumLayerC = glm::vec4(
        static_cast<float>(sanitized.mediumDetail.gradientDampenStrength),
        static_cast<float>(sanitized.mediumDetail.octaveRotationDegrees),
        0.0f,
        0.0f);
    uniforms.highLayerA = glm::vec4(
        static_cast<float>(sanitized.highDetail.wavelength),
        static_cast<float>(sanitized.highDetail.amplitude),
        static_cast<float>(sanitized.highDetail.bias),
        static_cast<float>(sanitized.highDetail.initialFrequency));
    uniforms.highLayerB = glm::vec4(
        static_cast<float>(sanitized.highDetail.initialAmplitude),
        static_cast<float>(sanitized.highDetail.octaveFrequencyScale),
        static_cast<float>(sanitized.highDetail.octaveAmplitudeScale),
        static_cast<float>(sanitized.highDetail.octaveCount));
    uniforms.highLayerC = glm::vec4(
        static_cast<float>(sanitized.highDetail.gradientDampenStrength),
        static_cast<float>(sanitized.highDetail.octaveRotationDegrees),
        0.0f,
        0.0f);
    uniforms.blendLayerA = glm::vec4(
        static_cast<float>(sanitized.blend.wavelength),
        static_cast<float>(sanitized.blend.initialFrequency),
        static_cast<float>(sanitized.blend.initialAmplitude),
        static_cast<float>(sanitized.blend.octaveFrequencyScale));
    uniforms.blendLayerB = glm::vec4(
        static_cast<float>(sanitized.blend.octaveAmplitudeScale),
        static_cast<float>(sanitized.blend.octaveCount),
        static_cast<float>(sanitized.blend.gradientDampenStrength),
        static_cast<float>(sanitized.blend.octaveRotationDegrees));
    uniforms.blendLayerC = glm::vec4(
        static_cast<float>(sanitized.blend.lowThreshold),
        static_cast<float>(sanitized.blend.highThreshold),
        static_cast<float>(sanitized.blend.lowTransitionWidth),
        static_cast<float>(sanitized.baseHeight));
    uniforms.highLayerC.z = static_cast<float>(sanitized.blend.highTransitionWidth);
    uniforms.dispatchParams = glm::uvec4(
        static_cast<std::uint32_t>(sliceIndex),
        AppConfig::Terrain::kHeightmapResolution,
        0u,
        0u);
    return true;
}

void QuadtreeMeshRenderer::addLeaf(const WorldGridQuadtreeLeafId& leafId, std::uint16_t sliceIndex)
{
    if (m_instanceCount >= m_instanceData.size())
    {
        return;
    }

    const auto [minCorner, maxCorner] = worldGridQuadtreeLeafBounds(leafId);
    (void)maxCorner;
    const glm::vec3 localMinCorner = localPositionFromWorldPosition(minCorner);

    m_instanceData[m_instanceCount++] = {
        .position{
            localMinCorner.x,
            localMinCorner.y,
            localMinCorner.z,
        },
        .packedMetadata = packMetadata(sliceIndex, worldGridQuadtreeLeafScalePow(leafId)),
    };
}

void QuadtreeMeshRenderer::upload(SDL_GPUCopyPass* copyPass)
{
    HELLO_PROFILE_SCOPE("QuadtreeMeshRenderer::Upload");

    if (m_pendingHeightmapGenerationCount > 0)
    {
        void* mappedGenerationUniforms = SDL_MapGPUTransferBuffer(m_device, m_heightmapGenerationTransferBuffer, true);
        std::memcpy(
            mappedGenerationUniforms,
            m_pendingHeightmapGenerations.data(),
            sizeof(HeightmapGenerationUniforms) * m_pendingHeightmapGenerationCount);
        SDL_UnmapGPUTransferBuffer(m_device, m_heightmapGenerationTransferBuffer);

        SDL_GPUTransferBufferLocation generationSource{};
        generationSource.transfer_buffer = m_heightmapGenerationTransferBuffer;
        SDL_GPUBufferRegion generationDestination{};
        generationDestination.buffer = m_heightmapGenerationBuffer;
        generationDestination.size = static_cast<Uint32>(sizeof(HeightmapGenerationUniforms) * m_pendingHeightmapGenerationCount);
        SDL_UploadToGPUBuffer(copyPass, &generationSource, &generationDestination, false);

        GpuHeightmapExtents* mappedExtents = static_cast<GpuHeightmapExtents*>(
            SDL_MapGPUTransferBuffer(m_device, m_heightmapExtentsInitTransferBuffer, true));
        for (std::uint16_t index = 0; index < m_pendingHeightmapGenerationCount; ++index)
        {
            const std::uint16_t sliceIndex = static_cast<std::uint16_t>(m_pendingHeightmapGenerations[index].dispatchParams.x);
            mappedExtents[sliceIndex] = {
                .minHeightCentimeters = kInitialMinHeightCentimeters,
                .maxHeightCentimeters = kInitialMaxHeightCentimeters,
            };
        }
        SDL_UnmapGPUTransferBuffer(m_device, m_heightmapExtentsInitTransferBuffer);

        for (std::uint16_t index = 0; index < m_pendingHeightmapGenerationCount; ++index)
        {
            const std::uint16_t sliceIndex = static_cast<std::uint16_t>(m_pendingHeightmapGenerations[index].dispatchParams.x);
            const Uint32 byteOffset = static_cast<Uint32>(sizeof(GpuHeightmapExtents) * sliceIndex);

            SDL_GPUTransferBufferLocation extentsSource{};
            extentsSource.transfer_buffer = m_heightmapExtentsInitTransferBuffer;
            extentsSource.offset = byteOffset;

            SDL_GPUBufferRegion extentsDestination{};
            extentsDestination.buffer = m_heightmapExtentsBuffer;
            extentsDestination.offset = byteOffset;
            extentsDestination.size = static_cast<Uint32>(sizeof(GpuHeightmapExtents));
            SDL_UploadToGPUBuffer(copyPass, &extentsSource, &extentsDestination, false);
        }
    }

    if (m_instanceCount > 0)
    {
        void* mappedInstances = SDL_MapGPUTransferBuffer(m_device, m_instanceTransferBuffer, true);
        std::memcpy(mappedInstances, m_instanceData.data(), sizeof(InstanceData) * m_instanceCount);
        SDL_UnmapGPUTransferBuffer(m_device, m_instanceTransferBuffer);

        SDL_GPUTransferBufferLocation instanceSource{};
        instanceSource.transfer_buffer = m_instanceTransferBuffer;
        SDL_GPUBufferRegion instanceDestination{};
        instanceDestination.buffer = m_instanceBuffer;
        instanceDestination.size = static_cast<Uint32>(sizeof(InstanceData) * m_instanceCount);
        SDL_UploadToGPUBuffer(copyPass, &instanceSource, &instanceDestination, true);

        const SDL_GPUIndexedIndirectDrawCommand drawCommand = makeDrawCommand(
            m_mainIndexCount,
            m_instanceCount,
            0,
            0,
            0
        );

        void* mappedIndirect = SDL_MapGPUTransferBuffer(m_device, m_indirectTransferBuffer, true);
        std::memcpy(mappedIndirect, &drawCommand, sizeof(drawCommand));
        SDL_UnmapGPUTransferBuffer(m_device, m_indirectTransferBuffer);

        SDL_GPUTransferBufferLocation indirectSource{};
        indirectSource.transfer_buffer = m_indirectTransferBuffer;
        SDL_GPUBufferRegion indirectDestination{};
        indirectDestination.buffer = m_indirectBuffer;
        indirectDestination.size = sizeof(SDL_GPUIndexedIndirectDrawCommand);
        SDL_UploadToGPUBuffer(copyPass, &indirectSource, &indirectDestination, true);
    }

}

void QuadtreeMeshRenderer::dispatchHeightmapGenerations(SDL_GPUCommandBuffer* commandBuffer)
{
    HELLO_PROFILE_SCOPE("QuadtreeMeshRenderer::DispatchHeightmapGenerations");

    m_lastDispatchedGenerationCount = 0;
    if (m_pendingHeightmapGenerationCount == 0)
    {
        return;
    }

    SDL_GPUStorageBufferReadWriteBinding storageBindings[2]{};
    storageBindings[0].buffer = m_heightmapBuffer;
    storageBindings[0].cycle = false;
    storageBindings[1].buffer = m_heightmapExtentsBuffer;
    storageBindings[1].cycle = false;

    SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(commandBuffer, nullptr, 0, storageBindings, 2);
    if (computePass == nullptr)
    {
        throwSdlError("Failed to begin terrain heightmap compute pass.");
    }

    SDL_BindGPUComputePipeline(computePass, m_heightmapComputePipeline);

    SDL_GPUBuffer* readonlyStorageBuffers[]{ m_heightmapGenerationBuffer };
    SDL_BindGPUComputeStorageBuffers(computePass, 0, readonlyStorageBuffers, 1);

    const std::uint32_t groupCountX =
        (AppConfig::Terrain::kHeightmapResolution + kHeightmapComputeThreadCountX - 1) / kHeightmapComputeThreadCountX;
    const std::uint32_t groupCountY =
        (AppConfig::Terrain::kHeightmapResolution + kHeightmapComputeThreadCountY - 1) / kHeightmapComputeThreadCountY;

    for (std::uint16_t index = 0; index < m_pendingHeightmapGenerationCount; ++index)
    {
        m_lastDispatchedLeafIds[index] = m_pendingGenerationLeafIds[index];
        m_lastDispatchedSlices[index] = static_cast<std::uint16_t>(m_pendingHeightmapGenerations[index].dispatchParams.x);
    }

    SDL_DispatchGPUCompute(computePass, groupCountX, groupCountY, m_pendingHeightmapGenerationCount);

    SDL_EndGPUComputePass(computePass);
    m_lastDispatchedGenerationCount = m_pendingHeightmapGenerationCount;
    m_pendingHeightmapGenerationCount = 0;
}

void QuadtreeMeshRenderer::queueHeightmapExtentsDownload(SDL_GPUCopyPass* copyPass)
{
    HELLO_PROFILE_SCOPE("QuadtreeMeshRenderer::QueueHeightmapExtentsDownload");

    m_pendingFenceReadbackSlot = UINT16_MAX;
    if (m_lastDispatchedGenerationCount == 0)
    {
        return;
    }

    for (std::size_t offset = 0; offset < m_pendingExtentsReadbacks.size(); ++offset)
    {
        const std::size_t slotIndex = (m_nextReadbackSlot + offset) % m_pendingExtentsReadbacks.size();
        PendingExtentsReadback& readback = m_pendingExtentsReadbacks[slotIndex];
        if (readback.fence != nullptr)
        {
            continue;
        }

        SDL_GPUBufferRegion source{};
        source.buffer = m_heightmapExtentsBuffer;
        source.offset = 0;
        source.size = static_cast<Uint32>(sizeof(GpuHeightmapExtents) * kHeightmapExtentsCount);

        SDL_GPUTransferBufferLocation destination{};
        destination.transfer_buffer = readback.transferBuffer;
        destination.offset = 0;
        SDL_DownloadFromGPUBuffer(copyPass, &source, &destination);

        readback.count = m_lastDispatchedGenerationCount;
        for (std::uint16_t index = 0; index < m_lastDispatchedGenerationCount; ++index)
        {
            readback.leafIds[index] = m_lastDispatchedLeafIds[index];
            readback.sliceIndices[index] = m_lastDispatchedSlices[index];
        }

        m_pendingFenceReadbackSlot = static_cast<std::uint16_t>(slotIndex);
        m_nextReadbackSlot = static_cast<std::uint16_t>((slotIndex + 1) % m_pendingExtentsReadbacks.size());
        break;
    }

    m_lastDispatchedGenerationCount = 0;
}

void QuadtreeMeshRenderer::attachSubmittedFence(SDL_GPUFence* fence)
{
    if (m_pendingFenceReadbackSlot == UINT16_MAX)
    {
        if (fence != nullptr)
        {
            SDL_ReleaseGPUFence(m_device, fence);
        }
        return;
    }

    PendingExtentsReadback& readback = m_pendingExtentsReadbacks[m_pendingFenceReadbackSlot];
    readback.fence = fence;
    m_pendingFenceReadbackSlot = UINT16_MAX;
}

void QuadtreeMeshRenderer::collectCompletedHeightmapExtents(std::vector<GeneratedHeightmapExtents>& completedExtents)
{
    HELLO_PROFILE_SCOPE("QuadtreeMeshRenderer::CollectCompletedHeightmapExtents");

    for (PendingExtentsReadback& readback : m_pendingExtentsReadbacks)
    {
        if (readback.fence == nullptr || !SDL_QueryGPUFence(m_device, readback.fence))
        {
            continue;
        }

        const GpuHeightmapExtents* mappedExtents = static_cast<const GpuHeightmapExtents*>(
            SDL_MapGPUTransferBuffer(m_device, readback.transferBuffer, false));
        for (std::uint16_t index = 0; index < readback.count; ++index)
        {
            const std::uint16_t sliceIndex = readback.sliceIndices[index];
            const GpuHeightmapExtents& gpuExtents = mappedExtents[sliceIndex];
            completedExtents.push_back({
                .leafId = readback.leafIds[index],
                .sliceIndex = sliceIndex,
                .extents = {
                    .minHeight = static_cast<float>(gpuExtents.minHeightCentimeters) * 0.01f,
                    .maxHeight = static_cast<float>(gpuExtents.maxHeightCentimeters) * 0.01f,
                },
            });
        }
        SDL_UnmapGPUTransferBuffer(m_device, readback.transferBuffer);
        SDL_ReleaseGPUFence(m_device, readback.fence);
        readback.fence = nullptr;
        readback.count = 0;
    }
}

void QuadtreeMeshRenderer::render(SDL_GPURenderPass* renderPass, SDL_GPUCommandBuffer* commandBuffer, const glm::mat4& viewProjection, const LightingSystem& lightingSystem) const
{
    HELLO_PROFILE_SCOPE("QuadtreeMeshRenderer::Render");

    if (m_instanceCount == 0)
    {
        return;
    }

    SDL_BindGPUGraphicsPipeline(renderPass, m_pipeline);

    const SDL_GPUBufferBinding vertexBinding{ m_vertexBuffer, 0 };
    SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

    SDL_GPUBufferBinding indexBinding{ m_indexBuffer, 0 };
    SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    SDL_GPUBuffer* storageBuffers[2]{ m_heightmapBuffer, m_instanceBuffer };
    SDL_BindGPUVertexStorageBuffers(renderPass, 0, storageBuffers, 2);

    TerrainUniforms uniforms{};
    uniforms.viewProjection = viewProjection;
    const glm::vec3 sunDirection = lightingSystem.sunDirection();
    uniforms.sunDirectionIntensity = glm::vec4(sunDirection, lightingSystem.sun().intensity);
    uniforms.sunColorAmbient = glm::vec4(lightingSystem.sun().color, AppConfig::Terrain::kAmbientLight);
    uniforms.terrainHeightParams = glm::vec4(m_terrainBaseHeight, m_terrainHeightAmplitude, 0.0f, 0.0f);
    SDL_PushGPUVertexUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

    SDL_DrawGPUIndexedPrimitivesIndirect(renderPass, m_indirectBuffer, 0, 1);
}

void QuadtreeMeshRenderer::createPipeline(const std::filesystem::path& shaderDirectory)
{
    SDL_GPUShader* vertexShader = createShader(shaderDirectory / "quadtree_mesh.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 1, 2);
    SDL_GPUShader* fragmentShader = createShader(shaderDirectory / "quadtree_mesh.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);

    SDL_GPUVertexBufferDescription vertexBufferDescription{};
    vertexBufferDescription.slot = 0;
    vertexBufferDescription.pitch = sizeof(Vertex);
    vertexBufferDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute vertexAttributes[2]{};
    vertexAttributes[0].location = 0;
    vertexAttributes[0].buffer_slot = 0;
    vertexAttributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    vertexAttributes[0].offset = offsetof(Vertex, localCoord);
    vertexAttributes[1].location = 1;
    vertexAttributes[1].buffer_slot = 0;
    vertexAttributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    vertexAttributes[1].offset = offsetof(Vertex, sampleCoord);

    SDL_GPUColorTargetBlendState blendState{};
    blendState.enable_blend = true;
    blendState.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    blendState.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blendState.color_blend_op = SDL_GPU_BLENDOP_ADD;
    blendState.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blendState.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blendState.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    blendState.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;

    SDL_GPUColorTargetDescription colorTargetDescription{};
    colorTargetDescription.format = m_colorFormat;
    colorTargetDescription.blend_state = blendState;

    SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.vertex_shader = vertexShader;
    pipelineInfo.fragment_shader = fragmentShader;
    pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipelineInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipelineInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
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
    pipelineInfo.vertex_input_state.num_vertex_attributes = 2;
    pipelineInfo.vertex_input_state.vertex_attributes = vertexAttributes;

    m_pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
    SDL_ReleaseGPUShader(m_device, fragmentShader);
    SDL_ReleaseGPUShader(m_device, vertexShader);

    if (m_pipeline == nullptr)
    {
        throwSdlError("Failed to create quadtree mesh graphics pipeline.");
    }
}

void QuadtreeMeshRenderer::createHeightmapComputePipeline(const std::filesystem::path& shaderDirectory)
{
    const std::vector<std::uint8_t> bytes = readShaderCode(shaderDirectory / "heightmap_generate.comp.spv");

    SDL_GPUComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.code_size = bytes.size();
    pipelineInfo.code = bytes.data();
    pipelineInfo.entrypoint = "main";
    pipelineInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    pipelineInfo.num_readonly_storage_buffers = 1;
    pipelineInfo.num_readwrite_storage_buffers = 2;
    pipelineInfo.num_uniform_buffers = 0;
    pipelineInfo.threadcount_x = kHeightmapComputeThreadCountX;
    pipelineInfo.threadcount_y = kHeightmapComputeThreadCountY;
    pipelineInfo.threadcount_z = kHeightmapComputeThreadCountZ;

    m_heightmapComputePipeline = SDL_CreateGPUComputePipeline(m_device, &pipelineInfo);
    if (m_heightmapComputePipeline == nullptr)
    {
        throwSdlError("Failed to create quadtree mesh compute pipeline.");
    }
}

void QuadtreeMeshRenderer::createStaticMeshResources()
{
    std::vector<Vertex> vertices;
    vertices.reserve(kMainGridVertexResolution * kMainGridVertexResolution);

    for (std::uint32_t z = 0; z < kMainGridVertexResolution; ++z)
    {
        for (std::uint32_t x = 0; x < kMainGridVertexResolution; ++x)
        {
            const float localX = static_cast<float>(x) + kRenderedLocalCoordOffset;
            const float localZ = static_cast<float>(z) + kRenderedLocalCoordOffset;
            const float sampleX = static_cast<float>(x) + kRenderedSampleCoordOffset;
            const float sampleZ = static_cast<float>(z) + kRenderedSampleCoordOffset;
            vertices.push_back({
                { localX, localZ },
                { sampleX, sampleZ },
            });
        }
    }

    std::vector<std::uint32_t> indices;
    indices.reserve(kMainGridQuadResolution * kMainGridQuadResolution * 6);

    for (std::uint32_t z = 0; z < kMainGridQuadResolution; ++z)
    {
        for (std::uint32_t x = 0; x < kMainGridQuadResolution; ++x)
        {
            const std::uint32_t topLeft = (z * kMainGridVertexResolution) + x;
            const std::uint32_t topRight = topLeft + 1;
            const std::uint32_t bottomLeft = topLeft + kMainGridVertexResolution;
            const std::uint32_t bottomRight = bottomLeft + 1;

            indices.push_back(topLeft);
            indices.push_back(bottomLeft);
            indices.push_back(topRight);
            indices.push_back(topRight);
            indices.push_back(bottomLeft);
            indices.push_back(bottomRight);
        }
    }

    m_mainIndexCount = static_cast<std::uint32_t>(indices.size());

    SDL_GPUBufferCreateInfo vertexInfo{};
    vertexInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertexInfo.size = static_cast<Uint32>(sizeof(Vertex) * vertices.size());
    m_vertexBuffer = SDL_CreateGPUBuffer(m_device, &vertexInfo);
    if (m_vertexBuffer == nullptr)
    {
        throwSdlError("Failed to create quadtree mesh vertex buffer.");
    }

    SDL_GPUTransferBufferCreateInfo vertexTransferInfo{};
    vertexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    vertexTransferInfo.size = vertexInfo.size;
    m_vertexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &vertexTransferInfo);
    if (m_vertexTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create quadtree mesh vertex transfer buffer.");
    }

    void* mappedVertices = SDL_MapGPUTransferBuffer(m_device, m_vertexTransferBuffer, false);
    std::memcpy(mappedVertices, vertices.data(), sizeof(Vertex) * vertices.size());
    SDL_UnmapGPUTransferBuffer(m_device, m_vertexTransferBuffer);

    SDL_GPUBufferCreateInfo indexInfo{};
    indexInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    indexInfo.size = static_cast<Uint32>(sizeof(std::uint32_t) * indices.size());
    m_indexBuffer = SDL_CreateGPUBuffer(m_device, &indexInfo);
    if (m_indexBuffer == nullptr)
    {
        throwSdlError("Failed to create quadtree mesh index buffer.");
    }

    SDL_GPUTransferBufferCreateInfo indexTransferInfo{};
    indexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    indexTransferInfo.size = indexInfo.size;
    m_indexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &indexTransferInfo);
    if (m_indexTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create quadtree mesh index transfer buffer.");
    }

    void* mappedIndices = SDL_MapGPUTransferBuffer(m_device, m_indexTransferBuffer, false);
    std::memcpy(mappedIndices, indices.data(), sizeof(std::uint32_t) * indices.size());
    SDL_UnmapGPUTransferBuffer(m_device, m_indexTransferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);

    SDL_GPUTransferBufferLocation vertexSource{};
    vertexSource.transfer_buffer = m_vertexTransferBuffer;
    SDL_GPUBufferRegion vertexDestination{};
    vertexDestination.buffer = m_vertexBuffer;
    vertexDestination.size = vertexInfo.size;
    SDL_UploadToGPUBuffer(copyPass, &vertexSource, &vertexDestination, false);

    SDL_GPUTransferBufferLocation indexSource{};
    indexSource.transfer_buffer = m_indexTransferBuffer;
    SDL_GPUBufferRegion indexDestination{};
    indexDestination.buffer = m_indexBuffer;
    indexDestination.size = indexInfo.size;
    SDL_UploadToGPUBuffer(copyPass, &indexSource, &indexDestination, false);

    SDL_EndGPUCopyPass(copyPass);
    if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
    {
        throwSdlError("Failed to upload quadtree mesh static buffers.");
    }
}

SDL_GPUIndexedIndirectDrawCommand QuadtreeMeshRenderer::makeDrawCommand(
    std::uint32_t indexCount,
    std::uint32_t instanceCount,
    std::uint32_t firstIndex,
    std::int32_t vertexOffset,
    std::uint32_t firstInstance
)
{
    SDL_GPUIndexedIndirectDrawCommand command{};
    command.num_indices = indexCount;
    command.num_instances = instanceCount;
    command.first_index = firstIndex;
    command.vertex_offset = vertexOffset;
    command.first_instance = firstInstance;
    return command;
}

std::uint32_t QuadtreeMeshRenderer::packMetadata(std::uint16_t sliceIndex, std::uint8_t scalePow)
{
    return
        static_cast<std::uint32_t>(sliceIndex) |
        (static_cast<std::uint32_t>(scalePow) << 16U);
}
