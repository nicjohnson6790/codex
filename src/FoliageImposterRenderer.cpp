#include "FoliageImposterRenderer.hpp"

#include "PerformanceCapture.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace
{
constexpr std::uint32_t kPackedEntrySize = sizeof(FoliagePackedInstance);
constexpr std::uint32_t kPageByteSize = FoliageConfig::kCandidateSlotCount * kPackedEntrySize;
}

void FoliageImposterRenderer::initialize(
    SDL_GPUDevice* device,
    SDL_GPUTextureFormat colorFormat,
    SDL_GPUTextureFormat depthFormat,
    const std::filesystem::path& shaderDirectory)
{
    initializeRendererBase(device, colorFormat, depthFormat);
    createMarkerVertexBuffer();
    createPagePoolBuffers();
    createIndirectAndDrawMetadataBuffers();
    createPipeline(shaderDirectory);
}

void FoliageImposterRenderer::shutdown()
{
    if (m_pipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
        m_pipeline = nullptr;
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
            2u,
            drawReference.liveCount,
            0u,
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
    std::memcpy(mappedIndirect, m_drawCommands.data(), sizeof(SDL_GPUIndirectDrawCommand) * m_drawCount);
    SDL_UnmapGPUTransferBuffer(m_device, m_indirectTransferBuffer);

    SDL_GPUTransferBufferLocation indirectSource{};
    indirectSource.transfer_buffer = m_indirectTransferBuffer;

    SDL_GPUBufferRegion indirectDestination{};
    indirectDestination.buffer = m_indirectBuffer;
    indirectDestination.size = static_cast<Uint32>(sizeof(SDL_GPUIndirectDrawCommand) * m_drawCount);

    SDL_UploadToGPUBuffer(copyPass, &indirectSource, &indirectDestination, true);
}

void FoliageImposterRenderer::render(
    SDL_GPURenderPass* renderPass,
    SDL_GPUCommandBuffer* commandBuffer,
    const glm::mat4& viewProjection,
    SDL_GPUBuffer* terrainHeightmapBuffer) const
{
    HELLO_PROFILE_SCOPE("FoliageImposterRenderer::Render");

    if (m_drawCount == 0 || terrainHeightmapBuffer == nullptr)
    {
        return;
    }

    Uniforms uniforms{};
    uniforms.viewProjection = viewProjection;
    SDL_BindGPUGraphicsPipeline(renderPass, m_pipeline);

    const SDL_GPUBufferBinding vertexBinding{ m_markerVertexBuffer, 0 };
    SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

    SDL_GPUBuffer* storageBuffers[]{
        m_drawMetadataBuffer,
        m_pagePoolBuffer,
        terrainHeightmapBuffer,
    };
    SDL_BindGPUVertexStorageBuffers(renderPass, 0, storageBuffers, 3);
    SDL_PushGPUVertexUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));
    SDL_DrawGPUPrimitivesIndirect(renderPass, m_indirectBuffer, 0, m_drawCount);
}

SDL_GPUIndirectDrawCommand FoliageImposterRenderer::makeDrawCommand(
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

void FoliageImposterRenderer::createPipeline(const std::filesystem::path& shaderDirectory)
{
    SDL_GPUShader* vertexShader = createShader(
        shaderDirectory / "foliage_imposter.vert.spv",
        SDL_GPU_SHADERSTAGE_VERTEX,
        1,
        3);
    SDL_GPUShader* fragmentShader = createShader(
        shaderDirectory / "foliage_imposter.frag.spv",
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        0);

    SDL_GPUVertexBufferDescription vertexBufferDescriptions[1]{};
    vertexBufferDescriptions[0].slot = 0;
    vertexBufferDescriptions[0].pitch = sizeof(Vertex);
    vertexBufferDescriptions[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute vertexAttributes[1]{};
    vertexAttributes[0].location = 0;
    vertexAttributes[0].buffer_slot = 0;
    vertexAttributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
    vertexAttributes[0].offset = offsetof(Vertex, endpoint);

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
    pipelineInfo.vertex_input_state.vertex_buffer_descriptions = vertexBufferDescriptions;
    pipelineInfo.vertex_input_state.num_vertex_attributes = 1;
    pipelineInfo.vertex_input_state.vertex_attributes = vertexAttributes;

    m_pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
    SDL_ReleaseGPUShader(m_device, fragmentShader);
    SDL_ReleaseGPUShader(m_device, vertexShader);
    if (m_pipeline == nullptr)
    {
        throwSdlError("Failed to create foliage impostor pipeline.");
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
    indirectBufferInfo.size = static_cast<Uint32>(sizeof(SDL_GPUIndirectDrawCommand) * m_drawCommands.size());
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

void FoliageImposterRenderer::createMarkerVertexBuffer()
{
    const std::array<Vertex, 2> vertices{ Vertex{ 0.0f }, Vertex{ 1.0f } };

    SDL_GPUBufferCreateInfo vertexBufferInfo{};
    vertexBufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertexBufferInfo.size = static_cast<Uint32>(sizeof(Vertex) * vertices.size());
    m_markerVertexBuffer = SDL_CreateGPUBuffer(m_device, &vertexBufferInfo);
    if (m_markerVertexBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage marker vertex buffer.");
    }

    SDL_GPUTransferBufferCreateInfo vertexTransferInfo{};
    vertexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    vertexTransferInfo.size = vertexBufferInfo.size;
    m_markerVertexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &vertexTransferInfo);
    if (m_markerVertexTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage marker vertex transfer buffer.");
    }

    void* mappedVertices = SDL_MapGPUTransferBuffer(m_device, m_markerVertexTransferBuffer, true);
    std::memcpy(mappedVertices, vertices.data(), sizeof(Vertex) * vertices.size());
    SDL_UnmapGPUTransferBuffer(m_device, m_markerVertexTransferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (commandBuffer == nullptr)
    {
        throwSdlError("Failed to acquire command buffer for foliage marker upload.");
    }

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    if (copyPass == nullptr)
    {
        throwSdlError("Failed to begin foliage marker copy pass.");
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
        throwSdlError("Failed to submit foliage marker upload.");
    }
}
