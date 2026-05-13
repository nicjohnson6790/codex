#include "FoliageCanopyRenderer.hpp"

#include "PerformanceCapture.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace
{
constexpr std::uint32_t kCanopyMeshQuadResolution = 16u;
constexpr std::uint32_t kCanopyMeshVertexResolution = kCanopyMeshQuadResolution + 1u;
constexpr std::array<float, 3> kCanopyMeshShellYValues{ 1.0f, 2.0f, 3.0f };
constexpr std::uint32_t kCanopyComputeThreadCountX = 64u;
constexpr std::uint32_t kCanopyComputeThreadCountY = 1u;
constexpr std::uint32_t kCanopyComputeThreadCountZ = 1u;

std::uint32_t packCanopyEdgeFadeStrengths(const std::array<std::uint8_t, 4>& strengths)
{
    return
        static_cast<std::uint32_t>(strengths[0]) |
        (static_cast<std::uint32_t>(strengths[1]) << 8u) |
        (static_cast<std::uint32_t>(strengths[2]) << 16u) |
        (static_cast<std::uint32_t>(strengths[3]) << 24u);
}
}

void FoliageCanopyRenderer::initialize(
    SDL_GPUDevice* device,
    SDL_GPUTextureFormat colorFormat,
    SDL_GPUTextureFormat depthFormat,
    const std::filesystem::path& shaderDirectory)
{
    initializeRendererBase(device, colorFormat, depthFormat);
    createMeshResources();
    createCanopyBuffers();
    createDrawBuffers();
    createPipeline(shaderDirectory);
    createComputePipeline(shaderDirectory);
}

void FoliageCanopyRenderer::shutdown()
{
    if (m_pipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
        m_pipeline = nullptr;
    }
    if (m_computePipeline != nullptr)
    {
        SDL_ReleaseGPUComputePipeline(m_device, m_computePipeline);
        m_computePipeline = nullptr;
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
    if (m_generationTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_generationTransferBuffer);
        m_generationTransferBuffer = nullptr;
    }
    if (m_generationBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_generationBuffer);
        m_generationBuffer = nullptr;
    }
    if (m_cellBitsetPoolBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_cellBitsetPoolBuffer);
        m_cellBitsetPoolBuffer = nullptr;
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
}

void FoliageCanopyRenderer::clear()
{
    m_drawCount = 0;
    m_pendingGenerationCount = 0;
}

void FoliageCanopyRenderer::setActiveCamera(const Position& cameraPosition)
{
    setActiveCameraPosition(cameraPosition);
}

bool FoliageCanopyRenderer::queueCellGeneration(
    const WorldGridQuadtreeLeafId& leafId,
    const WorldGridQuadtreeLeafId& terrainLeafId,
    std::uint16_t terrainSliceIndex,
    std::uint16_t canopySlotIndex,
    float waterLevel)
{
    if (m_pendingGenerationCount >= m_pendingGenerations.size() ||
        canopySlotIndex >= FoliageConfig::kCanopyCellPoolCapacity)
    {
        return false;
    }

    const auto [pageOrigin, pageMaxCorner] = worldGridQuadtreeLeafBounds(leafId);
    const auto [terrainOrigin, terrainMaxCorner] = worldGridQuadtreeLeafBounds(terrainLeafId);
    (void)pageMaxCorner;
    (void)terrainMaxCorner;

    const glm::dvec3 pageWorldMin = pageOrigin.worldPosition();
    const glm::dvec3 terrainWorldMin = terrainOrigin.worldPosition();
    const double terrainLeafSizeMeters = worldGridQuadtreeLeafSize(terrainLeafId);

    CellGenerationParams& params = m_pendingGenerations[m_pendingGenerationCount++];
    params.dispatchParams = glm::uvec4(
        canopySlotIndex,
        terrainSliceIndex,
        static_cast<std::uint32_t>(hashLeafId(leafId)),
        0u);
    params.terrainParams = glm::vec4(
        static_cast<float>(terrainLeafSizeMeters),
        static_cast<float>(pageWorldMin.x - terrainWorldMin.x),
        static_cast<float>(pageWorldMin.z - terrainWorldMin.z),
        waterLevel);
    params.worldParams = glm::vec4(
        static_cast<float>(pageWorldMin.x),
        static_cast<float>(pageWorldMin.z),
        0.0f,
        0.0f);
    return true;
}

void FoliageCanopyRenderer::addCanopyDraw(const FoliageCanopyDrawReference& drawReference)
{
    if (m_drawCount >= AppConfig::Foliage::kCanopyDrawCapacity)
    {
        return;
    }

    DrawMetadataGpu& draw = m_drawMetadata[m_drawCount++];
    const glm::vec3 patchOrigin = localPositionFromWorldPosition(drawReference.patchOrigin);
    const glm::vec3 terrainOrigin = localPositionFromWorldPosition(drawReference.terrainLeafOrigin);
    draw.patchOriginAndSize = glm::vec4(
        patchOrigin.x,
        patchOrigin.y,
        patchOrigin.z,
        drawReference.patchSizeMeters);
    draw.terrainOriginAndSize = glm::vec4(
        terrainOrigin.x,
        terrainOrigin.y,
        terrainOrigin.z,
        drawReference.terrainLeafSizeMeters);
    draw.terrainSliceData = glm::vec4(
        static_cast<float>(drawReference.terrainSliceIndex),
        0.0f,
        0.0f,
        0.0f);
    draw.patchSeedData = glm::uvec4(
        drawReference.patchSeed,
        packCanopyEdgeFadeStrengths(drawReference.edgeFadeStrengths),
        static_cast<std::uint32_t>(drawReference.drawAgeFrames),
        0u);

    for (std::uint32_t packedIndex = 0; packedIndex < FoliageConfig::kCanopyCellCountPerNode / 4u; ++packedIndex)
    {
        glm::uvec4 slots(UINT32_MAX);
        glm::uvec4 seeds(0u);
        for (std::uint32_t lane = 0; lane < 4u; ++lane)
        {
            const std::uint32_t cellIndex = (packedIndex * 4u) + lane;
            (&slots.x)[lane] = drawReference.cellSlotIndices[cellIndex];
            (&seeds.x)[lane] = drawReference.cellSeeds[cellIndex];
        }
        draw.cellSlots[packedIndex] = slots;
        draw.cellSeeds[packedIndex] = seeds;
    }
}

void FoliageCanopyRenderer::upload(SDL_GPUCopyPass* copyPass)
{
    HELLO_PROFILE_SCOPE("FoliageCanopyRenderer::Upload");

    if (m_pendingGenerationCount > 0)
    {
        void* mappedGenerations = SDL_MapGPUTransferBuffer(m_device, m_generationTransferBuffer, true);
        std::memcpy(
            mappedGenerations,
            m_pendingGenerations.data(),
            sizeof(CellGenerationParams) * m_pendingGenerationCount);
        SDL_UnmapGPUTransferBuffer(m_device, m_generationTransferBuffer);

        SDL_GPUTransferBufferLocation source{};
        source.transfer_buffer = m_generationTransferBuffer;

        SDL_GPUBufferRegion destination{};
        destination.buffer = m_generationBuffer;
        destination.size = static_cast<Uint32>(sizeof(CellGenerationParams) * m_pendingGenerationCount);
        SDL_UploadToGPUBuffer(copyPass, &source, &destination, false);
    }

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

    const SDL_GPUIndexedIndirectDrawCommand drawCommand = makeDrawCommand(
        m_indexCount,
        m_drawCount,
        0u,
        0,
        0u);
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

void FoliageCanopyRenderer::dispatchCellGenerations(SDL_GPUCommandBuffer* commandBuffer, SDL_GPUBuffer* terrainHeightmapBuffer)
{
    HELLO_PROFILE_SCOPE("FoliageCanopyRenderer::DispatchCellGenerations");

    if (m_pendingGenerationCount == 0 || terrainHeightmapBuffer == nullptr)
    {
        return;
    }

    const glm::uvec4 uniforms(m_pendingGenerationCount, 0u, 0u, 0u);
    SDL_PushGPUComputeUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

    SDL_GPUStorageBufferReadWriteBinding storageBinding{};
    storageBinding.buffer = m_cellBitsetPoolBuffer;
    storageBinding.cycle = false;

    SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(commandBuffer, nullptr, 0, &storageBinding, 1);
    if (computePass == nullptr)
    {
        throwSdlError("Failed to begin foliage canopy compute pass.");
    }

    SDL_BindGPUComputePipeline(computePass, m_computePipeline);
    SDL_GPUBuffer* readonlyStorageBuffers[]{
        m_generationBuffer,
        terrainHeightmapBuffer,
    };
    SDL_BindGPUComputeStorageBuffers(computePass, 0, readonlyStorageBuffers, 2);

    const std::uint32_t groupCountX =
        (FoliageConfig::kCanopyBitsetWordCount + kCanopyComputeThreadCountX - 1u) / kCanopyComputeThreadCountX;
    SDL_DispatchGPUCompute(computePass, groupCountX, 1u, m_pendingGenerationCount);
    SDL_EndGPUComputePass(computePass);

    m_pendingGenerationCount = 0;
}

void FoliageCanopyRenderer::render(
    SDL_GPURenderPass* renderPass,
    SDL_GPUCommandBuffer* commandBuffer,
    const glm::mat4& viewProjection,
    const LightingSystem& lightingSystem,
    SDL_GPUBuffer* terrainHeightmapBuffer) const
{
    HELLO_PROFILE_SCOPE("FoliageCanopyRenderer::Render");

    if (m_drawCount == 0 || terrainHeightmapBuffer == nullptr)
    {
        return;
    }

    Uniforms uniforms{};
    uniforms.viewProjection = viewProjection;
    uniforms.sunDirectionIntensity = glm::vec4(lightingSystem.sunDirection(), lightingSystem.sun().intensity);
    uniforms.canopyShellParams = glm::vec4(AppConfig::Foliage::kCanopyShellHeightOffsetMeters, 0.0f, 0.0f, 0.0f);
    SDL_PushGPUVertexUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));
    SDL_PushGPUFragmentUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

    SDL_BindGPUGraphicsPipeline(renderPass, m_pipeline);

    const SDL_GPUBufferBinding vertexBinding{ m_vertexBuffer, 0 };
    SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

    const SDL_GPUBufferBinding indexBinding{ m_indexBuffer, 0 };
    SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    SDL_GPUBuffer* vertexStorageBuffers[]{
        m_drawMetadataBuffer,
        m_cellBitsetPoolBuffer,
        terrainHeightmapBuffer,
    };
    SDL_BindGPUVertexStorageBuffers(renderPass, 0, vertexStorageBuffers, 3);

    SDL_GPUBuffer* fragmentStorageBuffers[]{
        m_drawMetadataBuffer,
        m_cellBitsetPoolBuffer,
    };
    SDL_BindGPUFragmentStorageBuffers(renderPass, 0, fragmentStorageBuffers, 2);

    SDL_DrawGPUIndexedPrimitivesIndirect(renderPass, m_indirectBuffer, 0, 1);
}

SDL_GPUIndexedIndirectDrawCommand FoliageCanopyRenderer::makeDrawCommand(
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

std::uint64_t FoliageCanopyRenderer::hashLeafId(const WorldGridQuadtreeLeafId& leafId)
{
    const std::uint64_t word0 = std::bit_cast<std::uint64_t>(leafId.gridX);
    const std::uint64_t word1 = std::bit_cast<std::uint64_t>(leafId.gridY);
    const std::uint64_t word2 = leafId.subdivisionPath;

    std::uint64_t hash = 0x9e3779b97f4a7c15ULL;
    hash ^= mix64(word0 + 0x9e3779b97f4a7c15ULL);
    hash = mix64(hash);
    hash ^= mix64(word1 + 0xbf58476d1ce4e5b9ULL);
    hash = mix64(hash);
    hash ^= mix64(word2 + 0x94d049bb133111ebULL);
    hash = mix64(hash);
    return hash;
}

std::uint64_t FoliageCanopyRenderer::mix64(std::uint64_t x)
{
    x ^= x >> 30U;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27U;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31U;
    return x;
}

void FoliageCanopyRenderer::createPipeline(const std::filesystem::path& shaderDirectory)
{
    SDL_GPUShader* vertexShader = createShader(
        shaderDirectory / "foliage_canopy.vert.spv",
        SDL_GPU_SHADERSTAGE_VERTEX,
        1,
        3);
    SDL_GPUShader* fragmentShader = createShader(
        shaderDirectory / "foliage_canopy.frag.spv",
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        1,
        2);

    SDL_GPUVertexBufferDescription vertexBufferDescription{};
    vertexBufferDescription.slot = 0;
    vertexBufferDescription.pitch = sizeof(Vertex);
    vertexBufferDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute vertexAttributes[2]{};
    vertexAttributes[0].location = 0;
    vertexAttributes[0].buffer_slot = 0;
    vertexAttributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    vertexAttributes[0].offset = offsetof(Vertex, uv);
    vertexAttributes[1].location = 1;
    vertexAttributes[1].buffer_slot = 0;
    vertexAttributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
    vertexAttributes[1].offset = offsetof(Vertex, shellY);

    SDL_GPUColorTargetBlendState blendState{};
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
    pipelineInfo.vertex_input_state.num_vertex_attributes = 2;
    pipelineInfo.vertex_input_state.vertex_attributes = vertexAttributes;

    m_pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
    SDL_ReleaseGPUShader(m_device, fragmentShader);
    SDL_ReleaseGPUShader(m_device, vertexShader);
    if (m_pipeline == nullptr)
    {
        throwSdlError("Failed to create foliage canopy pipeline.");
    }
}

void FoliageCanopyRenderer::createComputePipeline(const std::filesystem::path& shaderDirectory)
{
    const std::vector<std::uint8_t> bytes = readShaderCode(shaderDirectory / "foliage_canopy_generate.comp.spv");

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
    pipelineInfo.num_uniform_buffers = 1;
    pipelineInfo.threadcount_x = kCanopyComputeThreadCountX;
    pipelineInfo.threadcount_y = kCanopyComputeThreadCountY;
    pipelineInfo.threadcount_z = kCanopyComputeThreadCountZ;

    m_computePipeline = SDL_CreateGPUComputePipeline(m_device, &pipelineInfo);
    if (m_computePipeline == nullptr)
    {
        throwSdlError("Failed to create foliage canopy compute pipeline.");
    }
}

void FoliageCanopyRenderer::createCanopyBuffers()
{
    SDL_GPUBufferCreateInfo cellPoolInfo{};
    cellPoolInfo.usage =
        SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ |
        SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
        SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    cellPoolInfo.size = FoliageConfig::kCanopyCellPoolCapacity * FoliageConfig::kCanopyBitsetByteSize;
    m_cellBitsetPoolBuffer = SDL_CreateGPUBuffer(m_device, &cellPoolInfo);
    if (m_cellBitsetPoolBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage canopy bitset pool buffer.");
    }

    SDL_GPUBufferCreateInfo generationInfo{};
    generationInfo.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    generationInfo.size = static_cast<Uint32>(sizeof(CellGenerationParams) * m_pendingGenerations.size());
    m_generationBuffer = SDL_CreateGPUBuffer(m_device, &generationInfo);
    if (m_generationBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage canopy generation buffer.");
    }

    SDL_GPUTransferBufferCreateInfo generationTransferInfo{};
    generationTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    generationTransferInfo.size = generationInfo.size;
    m_generationTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &generationTransferInfo);
    if (m_generationTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage canopy generation transfer buffer.");
    }
}

void FoliageCanopyRenderer::createDrawBuffers()
{
    SDL_GPUBufferCreateInfo metadataBufferInfo{};
    metadataBufferInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    metadataBufferInfo.size = static_cast<Uint32>(sizeof(DrawMetadataGpu) * m_drawMetadata.size());
    m_drawMetadataBuffer = SDL_CreateGPUBuffer(m_device, &metadataBufferInfo);
    if (m_drawMetadataBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage canopy draw-metadata buffer.");
    }

    SDL_GPUTransferBufferCreateInfo metadataTransferInfo{};
    metadataTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    metadataTransferInfo.size = metadataBufferInfo.size;
    m_drawMetadataTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &metadataTransferInfo);
    if (m_drawMetadataTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage canopy draw-metadata transfer buffer.");
    }

    SDL_GPUBufferCreateInfo indirectBufferInfo{};
    indirectBufferInfo.usage = SDL_GPU_BUFFERUSAGE_INDIRECT;
    indirectBufferInfo.size = sizeof(SDL_GPUIndexedIndirectDrawCommand);
    m_indirectBuffer = SDL_CreateGPUBuffer(m_device, &indirectBufferInfo);
    if (m_indirectBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage canopy indirect buffer.");
    }

    SDL_GPUTransferBufferCreateInfo indirectTransferInfo{};
    indirectTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    indirectTransferInfo.size = indirectBufferInfo.size;
    m_indirectTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &indirectTransferInfo);
    if (m_indirectTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage canopy indirect transfer buffer.");
    }
}

void FoliageCanopyRenderer::createMeshResources()
{
    std::vector<Vertex> vertices;
    vertices.reserve(kCanopyMeshShellYValues.size() * kCanopyMeshVertexResolution * kCanopyMeshVertexResolution);
    for (float shellY : kCanopyMeshShellYValues)
    {
        for (std::uint32_t z = 0; z < kCanopyMeshVertexResolution; ++z)
        {
            for (std::uint32_t x = 0; x < kCanopyMeshVertexResolution; ++x)
            {
                vertices.push_back(Vertex{
                    {
                        static_cast<float>(x) / static_cast<float>(kCanopyMeshQuadResolution),
                        static_cast<float>(z) / static_cast<float>(kCanopyMeshQuadResolution),
                    },
                    shellY,
                });
            }
        }
    }

    std::vector<std::uint32_t> indices;
    indices.reserve(kCanopyMeshShellYValues.size() * kCanopyMeshQuadResolution * kCanopyMeshQuadResolution * 6u);
    for (std::uint32_t shellIndex = 0; shellIndex < kCanopyMeshShellYValues.size(); ++shellIndex)
    {
        const std::uint32_t shellVertexOffset =
            shellIndex * kCanopyMeshVertexResolution * kCanopyMeshVertexResolution;
        for (std::uint32_t z = 0; z < kCanopyMeshQuadResolution; ++z)
        {
            for (std::uint32_t x = 0; x < kCanopyMeshQuadResolution; ++x)
            {
                const std::uint32_t topLeft = shellVertexOffset + (z * kCanopyMeshVertexResolution) + x;
                const std::uint32_t topRight = topLeft + 1u;
                const std::uint32_t bottomLeft = topLeft + kCanopyMeshVertexResolution;
                const std::uint32_t bottomRight = bottomLeft + 1u;

                indices.push_back(topLeft);
                indices.push_back(bottomLeft);
                indices.push_back(topRight);

                indices.push_back(topRight);
                indices.push_back(bottomLeft);
                indices.push_back(bottomRight);
            }
        }
    }
    m_indexCount = static_cast<std::uint32_t>(indices.size());

    SDL_GPUBufferCreateInfo vertexBufferInfo{};
    vertexBufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertexBufferInfo.size = static_cast<Uint32>(sizeof(Vertex) * vertices.size());
    m_vertexBuffer = SDL_CreateGPUBuffer(m_device, &vertexBufferInfo);
    if (m_vertexBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage canopy vertex buffer.");
    }

    SDL_GPUTransferBufferCreateInfo vertexTransferInfo{};
    vertexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    vertexTransferInfo.size = vertexBufferInfo.size;
    m_vertexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &vertexTransferInfo);
    if (m_vertexTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage canopy vertex transfer buffer.");
    }

    SDL_GPUBufferCreateInfo indexBufferInfo{};
    indexBufferInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    indexBufferInfo.size = static_cast<Uint32>(sizeof(std::uint32_t) * indices.size());
    m_indexBuffer = SDL_CreateGPUBuffer(m_device, &indexBufferInfo);
    if (m_indexBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage canopy index buffer.");
    }

    SDL_GPUTransferBufferCreateInfo indexTransferInfo{};
    indexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    indexTransferInfo.size = indexBufferInfo.size;
    m_indexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &indexTransferInfo);
    if (m_indexTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create foliage canopy index transfer buffer.");
    }

    void* mappedVertices = SDL_MapGPUTransferBuffer(m_device, m_vertexTransferBuffer, true);
    std::memcpy(mappedVertices, vertices.data(), sizeof(Vertex) * vertices.size());
    SDL_UnmapGPUTransferBuffer(m_device, m_vertexTransferBuffer);

    void* mappedIndices = SDL_MapGPUTransferBuffer(m_device, m_indexTransferBuffer, true);
    std::memcpy(mappedIndices, indices.data(), sizeof(std::uint32_t) * indices.size());
    SDL_UnmapGPUTransferBuffer(m_device, m_indexTransferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (commandBuffer == nullptr)
    {
        throwSdlError("Failed to acquire command buffer for foliage canopy mesh upload.");
    }

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    if (copyPass == nullptr)
    {
        throwSdlError("Failed to begin foliage canopy mesh copy pass.");
    }

    SDL_GPUTransferBufferLocation vertexSource{};
    vertexSource.transfer_buffer = m_vertexTransferBuffer;
    SDL_GPUBufferRegion vertexDestination{};
    vertexDestination.buffer = m_vertexBuffer;
    vertexDestination.size = vertexBufferInfo.size;
    SDL_UploadToGPUBuffer(copyPass, &vertexSource, &vertexDestination, true);

    SDL_GPUTransferBufferLocation indexSource{};
    indexSource.transfer_buffer = m_indexTransferBuffer;
    SDL_GPUBufferRegion indexDestination{};
    indexDestination.buffer = m_indexBuffer;
    indexDestination.size = indexBufferInfo.size;
    SDL_UploadToGPUBuffer(copyPass, &indexSource, &indexDestination, true);

    SDL_EndGPUCopyPass(copyPass);
    if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
    {
        throwSdlError("Failed to submit foliage canopy mesh upload.");
    }
}
