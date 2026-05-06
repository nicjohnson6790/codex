#include "NearbyFoliageRenderer.hpp"

#include "PerformanceCapture.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace
{
constexpr std::uint32_t kDecodeComputeThreadCountX = 64u;
constexpr std::uint32_t kDecodeComputeThreadCountY = 1u;
constexpr std::uint32_t kDecodeComputeThreadCountZ = 1u;
}

void NearbyFoliageRenderer::initialize(
    SDL_GPUDevice* device,
    SDL_GPUTextureFormat colorFormat,
    SDL_GPUTextureFormat depthFormat,
    const std::filesystem::path& shaderDirectory)
{
    initializeRendererBase(device, colorFormat, depthFormat);
    createMarkerVertexBuffer();
    createDecodeBuffers();
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
    if (m_markerVertexTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_markerVertexTransferBuffer);
        m_markerVertexTransferBuffer = nullptr;
    }
    if (m_markerVertexBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_markerVertexBuffer);
        m_markerVertexBuffer = nullptr;
    }

    resetTransientState();
    m_decodedPages.fill({});
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
            if ((instance.flags & NearbyFoliageInstance_Resident) == 0u)
            {
                continue;
            }

            const float deltaX = instance.localX - localNearX;
            const float deltaZ = instance.localZ - localNearZ;
            if ((deltaX * deltaX) + (deltaZ * deltaZ) > radiusSquared)
            {
                continue;
            }

            if (m_drawCount >= m_drawInstances.size())
            {
                return;
            }

            m_drawInstances[m_drawCount++] = {
                .pageOriginAndSlice = glm::vec4(
                    localPageOrigin.x,
                    localPageOrigin.y,
                    localPageOrigin.z,
                    static_cast<float>(terrainSliceIndex)),
                .localOffsetAndMesh = glm::vec4(
                    instance.localX,
                    instance.localZ,
                    0.0f,
                    static_cast<float>(instance.meshId)),
            };
        }
    }
}

void NearbyFoliageRenderer::upload(SDL_GPUCopyPass* copyPass)
{
    HELLO_PROFILE_SCOPE("NearbyFoliageRenderer::Upload");

    if (m_drawCount == 0)
    {
        return;
    }

    void* mappedInstances = SDL_MapGPUTransferBuffer(m_device, m_drawInstanceTransferBuffer, true);
    std::memcpy(mappedInstances, m_drawInstances.data(), sizeof(DrawInstanceGpu) * m_drawCount);
    SDL_UnmapGPUTransferBuffer(m_device, m_drawInstanceTransferBuffer);

    SDL_GPUTransferBufferLocation instanceSource{};
    instanceSource.transfer_buffer = m_drawInstanceTransferBuffer;
    SDL_GPUBufferRegion instanceDestination{};
    instanceDestination.buffer = m_drawInstanceBuffer;
    instanceDestination.size = static_cast<Uint32>(sizeof(DrawInstanceGpu) * m_drawCount);
    SDL_UploadToGPUBuffer(copyPass, &instanceSource, &instanceDestination, true);

    const SDL_GPUIndirectDrawCommand drawCommand = makeDrawCommand(2u, m_drawCount, 0u, 0u);
    void* mappedIndirect = SDL_MapGPUTransferBuffer(m_device, m_indirectTransferBuffer, true);
    std::memcpy(mappedIndirect, &drawCommand, sizeof(drawCommand));
    SDL_UnmapGPUTransferBuffer(m_device, m_indirectTransferBuffer);

    SDL_GPUTransferBufferLocation indirectSource{};
    indirectSource.transfer_buffer = m_indirectTransferBuffer;
    SDL_GPUBufferRegion indirectDestination{};
    indirectDestination.buffer = m_indirectBuffer;
    indirectDestination.size = sizeof(SDL_GPUIndirectDrawCommand);
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
    SDL_GPUBuffer* terrainHeightmapBuffer) const
{
    HELLO_PROFILE_SCOPE("NearbyFoliageRenderer::Render");

    if (m_drawCount == 0 || terrainHeightmapBuffer == nullptr)
    {
        return;
    }

    Uniforms uniforms{};
    uniforms.viewProjection = viewProjection;
    SDL_PushGPUVertexUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

    SDL_BindGPUGraphicsPipeline(renderPass, m_pipeline);

    const SDL_GPUBufferBinding vertexBinding{ m_markerVertexBuffer, 0 };
    SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

    SDL_GPUBuffer* storageBuffers[]{
        m_drawInstanceBuffer,
        terrainHeightmapBuffer,
    };
    SDL_BindGPUVertexStorageBuffers(renderPass, 0, storageBuffers, 2);
    SDL_DrawGPUPrimitivesIndirect(renderPass, m_indirectBuffer, 0, 1);
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

SDL_GPUIndirectDrawCommand NearbyFoliageRenderer::makeDrawCommand(
    std::uint32_t vertexCount,
    std::uint32_t instanceCount,
    std::uint32_t firstVertex,
    std::uint32_t firstInstance)
{
    SDL_GPUIndirectDrawCommand command{};
    command.num_vertices = vertexCount;
    command.num_instances = instanceCount;
    command.first_vertex = firstVertex;
    command.first_instance = firstInstance;
    return command;
}

void NearbyFoliageRenderer::createPipeline(const std::filesystem::path& shaderDirectory)
{
    SDL_GPUShader* vertexShader = createShader(
        shaderDirectory / "nearby_foliage.vert.spv",
        SDL_GPU_SHADERSTAGE_VERTEX,
        1,
        2);
    SDL_GPUShader* fragmentShader = createShader(
        shaderDirectory / "nearby_foliage.frag.spv",
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        0);

    SDL_GPUVertexBufferDescription vertexBufferDescription{};
    vertexBufferDescription.slot = 0;
    vertexBufferDescription.pitch = sizeof(Vertex);
    vertexBufferDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute vertexAttribute{};
    vertexAttribute.location = 0;
    vertexAttribute.buffer_slot = 0;
    vertexAttribute.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
    vertexAttribute.offset = offsetof(Vertex, endpoint);

    SDL_GPUColorTargetBlendState blendState{};
    blendState.enable_blend = true;
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
    pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_LINELIST;
    pipelineInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipelineInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipelineInfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipelineInfo.rasterizer_state.enable_depth_clip = true;
    pipelineInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pipelineInfo.depth_stencil_state.enable_depth_test = true;
    pipelineInfo.depth_stencil_state.enable_depth_write = false;
    pipelineInfo.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    pipelineInfo.target_info.num_color_targets = 1;
    pipelineInfo.target_info.color_target_descriptions = &colorTargetDescription;
    pipelineInfo.target_info.has_depth_stencil_target = true;
    pipelineInfo.target_info.depth_stencil_format = m_depthFormat;
    pipelineInfo.vertex_input_state.num_vertex_buffers = 1;
    pipelineInfo.vertex_input_state.vertex_buffer_descriptions = &vertexBufferDescription;
    pipelineInfo.vertex_input_state.num_vertex_attributes = 1;
    pipelineInfo.vertex_input_state.vertex_attributes = &vertexAttribute;

    m_pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
    SDL_ReleaseGPUShader(m_device, fragmentShader);
    SDL_ReleaseGPUShader(m_device, vertexShader);
    if (m_pipeline == nullptr)
    {
        throwSdlError("Failed to create nearby foliage marker pipeline.");
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

void NearbyFoliageRenderer::createMarkerVertexBuffer()
{
    const std::array<Vertex, 2> vertices{ Vertex{ 0.0f }, Vertex{ 1.0f } };

    SDL_GPUBufferCreateInfo vertexBufferInfo{};
    vertexBufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertexBufferInfo.size = static_cast<Uint32>(sizeof(Vertex) * vertices.size());
    m_markerVertexBuffer = SDL_CreateGPUBuffer(m_device, &vertexBufferInfo);
    if (m_markerVertexBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage marker vertex buffer.");
    }

    SDL_GPUTransferBufferCreateInfo vertexTransferInfo{};
    vertexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    vertexTransferInfo.size = vertexBufferInfo.size;
    m_markerVertexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &vertexTransferInfo);
    if (m_markerVertexTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create nearby foliage marker vertex transfer buffer.");
    }

    void* mappedVertices = SDL_MapGPUTransferBuffer(m_device, m_markerVertexTransferBuffer, true);
    std::memcpy(mappedVertices, vertices.data(), sizeof(Vertex) * vertices.size());
    SDL_UnmapGPUTransferBuffer(m_device, m_markerVertexTransferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (commandBuffer == nullptr)
    {
        throwSdlError("Failed to acquire command buffer for nearby foliage marker upload.");
    }

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    if (copyPass == nullptr)
    {
        throwSdlError("Failed to begin nearby foliage marker copy pass.");
    }

    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = m_markerVertexTransferBuffer;
    SDL_GPUBufferRegion destination{};
    destination.buffer = m_markerVertexBuffer;
    destination.size = vertexBufferInfo.size;
    SDL_UploadToGPUBuffer(copyPass, &source, &destination, true);
    SDL_EndGPUCopyPass(copyPass);

    if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
    {
        throwSdlError("Failed to submit nearby foliage marker upload.");
    }
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
    indirectInfo.size = sizeof(SDL_GPUIndirectDrawCommand);
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
}

void NearbyFoliageRenderer::resetTransientState()
{
    m_pendingDecodeCount = 0u;
    m_lastDispatchedDecodeCount = 0u;
    m_pendingFenceReadbackCount = 0u;
    m_topologyHintCount = 0u;
    m_drawCount = 0u;
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
