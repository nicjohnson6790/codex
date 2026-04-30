#include "QuadtreeWaterMeshRenderer.hpp"

#include "AppConfig.hpp"
#include "PerformanceCapture.hpp"

#include <SDL3/SDL_stdinc.h>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
constexpr std::uint32_t kWaterComputeThreadCountX = 16;
constexpr std::uint32_t kWaterComputeThreadCountY = 16;
constexpr std::uint32_t kWaterComputeThreadCountZ = 1;
constexpr std::uint32_t kWaterFftThreadCountX = 256;
constexpr std::uint32_t kWaterFftThreadCountY = 1;
constexpr std::uint32_t kWaterFftThreadCountZ = 1;
constexpr SDL_GPUTextureFormat kWaterTextureFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;

std::uint32_t fftStageCountForResolution(std::uint32_t resolution)
{
    std::uint32_t stages = 0;
    while ((1u << stages) < resolution)
    {
        ++stages;
    }

    return stages;
}

glm::vec4 buildFoamGenerationParams(const WaterSettings& settings)
{
    return glm::vec4(
        settings.crestFoamEnabled ? std::max(settings.crestFoamAmount, 0.0f) : 0.0f,
        std::max(settings.crestFoamThreshold, 0.0f),
        std::max(settings.crestFoamSoftness, 1.0e-4f),
        std::max(settings.crestFoamSlopeStart, 0.0f));
}

glm::vec4 buildFoamHistoryParams(const WaterSettings& settings, bool hasValidFoamHistory)
{
    return glm::vec4(
        std::max(settings.crestFoamDecayRate, 0.0f),
        hasValidFoamHistory ? 1.0f : 0.0f,
        0.0f,
        0.0f);
}
}

void QuadtreeWaterMeshRenderer::initialize(
    SDL_GPUDevice* device,
    SDL_GPUTextureFormat colorFormat,
    SDL_GPUTextureFormat depthFormat,
    const std::filesystem::path& shaderDirectory)
{
    initializeRendererBase(device, colorFormat, depthFormat);
    m_settings = makeDefaultWaterSettings();
    createInstanceBuffer();
    createMesh();
    createWorkingBuffers();
    createWaterTextures();
    createWaterSampler();
    createPipeline(shaderDirectory);
    createWaterComputePipelines(shaderDirectory);
}

void QuadtreeWaterMeshRenderer::shutdown()
{
    if (m_initializeSpectrumPipeline != nullptr)
    {
        SDL_ReleaseGPUComputePipeline(m_device, m_initializeSpectrumPipeline);
        m_initializeSpectrumPipeline = nullptr;
    }
    if (m_buildMapsPipeline != nullptr)
    {
        SDL_ReleaseGPUComputePipeline(m_device, m_buildMapsPipeline);
        m_buildMapsPipeline = nullptr;
    }
    if (m_fftStagePipeline != nullptr)
    {
        SDL_ReleaseGPUComputePipeline(m_device, m_fftStagePipeline);
        m_fftStagePipeline = nullptr;
    }
    if (m_spectrumUpdatePipeline != nullptr)
    {
        SDL_ReleaseGPUComputePipeline(m_device, m_spectrumUpdatePipeline);
        m_spectrumUpdatePipeline = nullptr;
    }
    if (m_pipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
        m_pipeline = nullptr;
    }

    destroyWaterSampler();
    destroyWaterTextures();
    destroyWorkingBuffers();
    destroyMesh();
    destroyInstanceBuffer();
    clear();
}

void QuadtreeWaterMeshRenderer::clear()
{
    m_instances.instanceCount = 0;
}

void QuadtreeWaterMeshRenderer::setActiveCamera(const Position& cameraPosition)
{
    setActiveCameraPosition(cameraPosition);
}

void QuadtreeWaterMeshRenderer::setSettings(const WaterSettings& settings)
{
    if (std::memcmp(&m_settings, &settings, sizeof(WaterSettings)) != 0)
    {
        m_initialSpectrumDirty = true;
        m_hasValidFoamHistory = false;
    }
    m_settings = settings;
}

void QuadtreeWaterMeshRenderer::rebuildMesh()
{
    if (m_device == nullptr)
    {
        return;
    }

    if (!SDL_WaitForGPUIdle(m_device))
    {
        throwSdlError("Failed to wait for GPU idle before rebuilding water meshes.");
    }

    destroyMesh();
    createMesh();
}

void QuadtreeWaterMeshRenderer::addLeaf(
    const WorldGridQuadtreeLeafId& leafId,
    const Position& leafOrigin,
    double leafSizeMeters,
    std::uint8_t quadtreeLodHint,
    bool hasTerrainSlice,
    std::uint16_t terrainSliceIndex,
    std::uint32_t bandMask)
{
    (void)leafId;

    if (m_instances.instanceCount >= AppConfig::Water::kMaxWaterInstances)
    {
        return;
    }

    const glm::vec3 localOrigin = localPositionFromWorldPosition(leafOrigin);

    InstanceData& instance = m_instances.instances[m_instances.instanceCount++];
    instance.position[0] = localOrigin.x;
    instance.position[1] = localOrigin.y;
    instance.position[2] = localOrigin.z;
    instance.packedMetadata = packMetadata(quadtreeLodHint, bandMask);
    instance.leafParams = glm::vec4(
        static_cast<float>(leafSizeMeters),
        m_settings.waterLevel,
        static_cast<float>(terrainSliceIndex),
        hasTerrainSlice ? 1.0f : 0.0f);
}

void QuadtreeWaterMeshRenderer::upload(SDL_GPUCopyPass* copyPass)
{
    HELLO_PROFILE_SCOPE_GROUPS("QuadtreeWaterMeshRenderer::Upload", ProfileScopeGroup::Renderer);

    if (m_instances.instanceCount == 0)
    {
        return;
    }

    void* mappedInstances = SDL_MapGPUTransferBuffer(m_device, m_instances.instanceTransferBuffer, true);
    std::memcpy(
        mappedInstances,
        m_instances.instances.data(),
        sizeof(InstanceData) * m_instances.instanceCount);
    SDL_UnmapGPUTransferBuffer(m_device, m_instances.instanceTransferBuffer);

    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = m_instances.instanceTransferBuffer;

    SDL_GPUBufferRegion destination{};
    destination.buffer = m_instances.instanceBuffer;
    destination.size = static_cast<Uint32>(sizeof(InstanceData) * m_instances.instanceCount);
    SDL_UploadToGPUBuffer(copyPass, &source, &destination, true);
}

void QuadtreeWaterMeshRenderer::dispatchWaterSimulation(
    SDL_GPUCommandBuffer* commandBuffer,
    float timeSeconds,
    std::uint64_t frameIndex)
{
    HELLO_PROFILE_SCOPE_GROUPS("QuadtreeWaterMeshRenderer::DispatchWaterSimulation", ProfileScopeGroup::Renderer);
    (void)frameIndex;

    if (!m_settings.enabled ||
        m_settings.cascadeCount == 0 ||
        m_initializeSpectrumPipeline == nullptr ||
        m_spectrumUpdatePipeline == nullptr ||
        m_fftStagePipeline == nullptr ||
        m_buildMapsPipeline == nullptr ||
        m_workingBuffers.initialSpectrum == nullptr ||
        m_workingBuffers.displacementSpectrumPing == nullptr ||
        m_workingBuffers.displacementSpectrumPong == nullptr ||
        m_workingBuffers.slopeSpectrumPing == nullptr ||
        m_workingBuffers.slopeSpectrumPong == nullptr ||
        m_displacementTexture == nullptr ||
        m_slopeTexture == nullptr ||
        m_foamHistoryReadTexture == nullptr ||
        m_foamHistoryWriteTexture == nullptr ||
        m_waterSampler == nullptr)
    {
        return;
    }

    const WaterSimulationUniforms buildUniforms = buildSimulationUniforms(timeSeconds, 0u, 0u, 0u);
    if (m_initialSpectrumDirty)
    {
        dispatchInitializeSpectrum(commandBuffer, buildUniforms);
        m_initialSpectrumDirty = false;
    }
    dispatchSpectrumUpdate(commandBuffer, buildUniforms);
    dispatchFftStages(commandBuffer, timeSeconds);
    dispatchBuildMaps(commandBuffer, buildUniforms);
}

void QuadtreeWaterMeshRenderer::render(
    SDL_GPURenderPass* renderPass,
    SDL_GPUCommandBuffer* commandBuffer,
    const glm::mat4& viewProjection,
    const LightingSystem& lightingSystem,
    const SkyboxRenderer& skyboxRenderer,
    float timeSeconds,
    SDL_GPUBuffer* terrainHeightmapBuffer) const
{
    HELLO_PROFILE_SCOPE_GROUPS("QuadtreeWaterMeshRenderer::Render", ProfileScopeGroup::Renderer);

    if (!m_settings.enabled ||
        totalInstanceCount() == 0 ||
        m_pipeline == nullptr ||
        m_displacementTexture == nullptr ||
        m_slopeTexture == nullptr ||
        m_foamHistoryReadTexture == nullptr ||
        m_waterSampler == nullptr ||
        skyboxRenderer.cubemapTexture() == nullptr ||
        skyboxRenderer.atmosphereLutTexture() == nullptr ||
        skyboxRenderer.cubemapSampler() == nullptr ||
        skyboxRenderer.atmosphereSampler() == nullptr ||
        terrainHeightmapBuffer == nullptr)
    {
        return;
    }

    SDL_BindGPUGraphicsPipeline(renderPass, m_pipeline);

    const WaterUniforms uniforms = buildWaterUniforms(viewProjection, lightingSystem, skyboxRenderer, timeSeconds);
    SDL_PushGPUVertexUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));
    SDL_PushGPUFragmentUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

    const SDL_GPUTextureSamplerBinding vertexSamplerBindings[1]{
        { m_displacementTexture, m_waterSampler },
    };
    SDL_BindGPUVertexSamplers(renderPass, 0, vertexSamplerBindings, 1);

    const SDL_GPUTextureSamplerBinding fragmentSamplerBindings[5]{
        { m_displacementTexture, m_waterSampler },
        { m_slopeTexture, m_waterSampler },
        { m_foamHistoryReadTexture, m_waterSampler },
        { skyboxRenderer.cubemapTexture(), skyboxRenderer.cubemapSampler() },
        { skyboxRenderer.atmosphereLutTexture(), skyboxRenderer.atmosphereSampler() },
    };
    SDL_BindGPUFragmentSamplers(renderPass, 0, fragmentSamplerBindings, 5);

    if (m_mesh.vertexBuffer == nullptr || m_mesh.indexBuffer == nullptr || m_instances.instanceCount == 0)
    {
        return;
    }

    const SDL_GPUBufferBinding vertexBinding{ m_mesh.vertexBuffer, 0 };
    SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

    const SDL_GPUBufferBinding indexBinding{ m_mesh.indexBuffer, 0 };
    SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    SDL_GPUBuffer* storageBuffers[]{ terrainHeightmapBuffer, m_instances.instanceBuffer };
    SDL_BindGPUVertexStorageBuffers(renderPass, 0, storageBuffers, 2);
    SDL_GPUBuffer* fragmentStorageBuffers[]{ terrainHeightmapBuffer };
    SDL_BindGPUFragmentStorageBuffers(renderPass, 0, fragmentStorageBuffers, 1);

    SDL_DrawGPUIndexedPrimitives(
        renderPass,
        m_mesh.indexCount,
        m_instances.instanceCount,
        0,
        0,
        0);
}

std::uint32_t QuadtreeWaterMeshRenderer::instanceCount() const
{
    return m_instances.instanceCount;
}

std::uint32_t QuadtreeWaterMeshRenderer::totalInstanceCount() const
{
    return m_instances.instanceCount;
}

std::uint32_t QuadtreeWaterMeshRenderer::packMetadata(
    std::uint8_t quadtreeLodHint,
    std::uint32_t bandMask)
{
    return
        (static_cast<std::uint32_t>(quadtreeLodHint) & 0xFFu) |
        ((bandMask & 0xFFFFu) << 16);
}

void QuadtreeWaterMeshRenderer::createPipeline(const std::filesystem::path& shaderDirectory)
{
    SDL_GPUShader* vertexShader = createShader(shaderDirectory / "water_mesh.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 1, 2, 1);
    SDL_GPUShader* fragmentShader = createShader(shaderDirectory / "water_mesh.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1, 5);

    SDL_GPUVertexBufferDescription vertexBufferDescription{};
    vertexBufferDescription.slot = 0;
    vertexBufferDescription.pitch = sizeof(Vertex);
    vertexBufferDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute vertexAttribute{};
    vertexAttribute.location = 0;
    vertexAttribute.buffer_slot = 0;
    vertexAttribute.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    vertexAttribute.offset = offsetof(Vertex, localCoord);

    SDL_GPUColorTargetDescription colorTargetDescription{};
    colorTargetDescription.format = m_colorFormat;

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
    pipelineInfo.vertex_input_state.num_vertex_attributes = 1;
    pipelineInfo.vertex_input_state.vertex_attributes = &vertexAttribute;

    m_pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
    SDL_ReleaseGPUShader(m_device, fragmentShader);
    SDL_ReleaseGPUShader(m_device, vertexShader);

    if (m_pipeline == nullptr)
    {
        throwSdlError("Failed to create water graphics pipeline.");
    }
}

void QuadtreeWaterMeshRenderer::createWaterComputePipelines(const std::filesystem::path& shaderDirectory)
{
    const auto createComputePipeline = [this](const std::filesystem::path& path,
                                           std::uint32_t samplerCount,
                                           std::uint32_t readonlyStorageTextureCount,
                                           std::uint32_t readonlyStorageBufferCount,
                                           std::uint32_t readwriteStorageTextureCount,
                                           std::uint32_t readwriteStorageBufferCount,
                                           std::uint32_t threadCountX,
                                           std::uint32_t threadCountY,
                                           std::uint32_t threadCountZ) -> SDL_GPUComputePipeline*
    {
        const std::vector<std::uint8_t> bytes = readShaderCode(path);

        SDL_GPUComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.code_size = bytes.size();
        pipelineInfo.code = bytes.data();
        pipelineInfo.entrypoint = "main";
        pipelineInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
        pipelineInfo.num_samplers = samplerCount;
        pipelineInfo.num_readonly_storage_textures = readonlyStorageTextureCount;
        pipelineInfo.num_readonly_storage_buffers = readonlyStorageBufferCount;
        pipelineInfo.num_readwrite_storage_textures = readwriteStorageTextureCount;
        pipelineInfo.num_readwrite_storage_buffers = readwriteStorageBufferCount;
        pipelineInfo.num_uniform_buffers = 1;
        pipelineInfo.threadcount_x = threadCountX;
        pipelineInfo.threadcount_y = threadCountY;
        pipelineInfo.threadcount_z = threadCountZ;

        SDL_GPUComputePipeline* pipeline = SDL_CreateGPUComputePipeline(m_device, &pipelineInfo);
        if (pipeline == nullptr)
        {
            throwSdlError(("Failed to create water compute pipeline: " + path.string()).c_str());
        }

        return pipeline;
    };

    m_initializeSpectrumPipeline = createComputePipeline(
        shaderDirectory / "water_initialize_spectrum.comp.spv",
        0, 0, 0, 0, 1,
        kWaterComputeThreadCountX, kWaterComputeThreadCountY, kWaterComputeThreadCountZ);
    m_spectrumUpdatePipeline = createComputePipeline(
        shaderDirectory / "water_spectrum_update.comp.spv",
        0, 0, 1, 0, 2,
        kWaterComputeThreadCountX, kWaterComputeThreadCountY, kWaterComputeThreadCountZ);
    m_fftStagePipeline = createComputePipeline(
        shaderDirectory / "water_fft_stage.comp.spv",
        0, 0, 1, 0, 1,
        kWaterFftThreadCountX, kWaterFftThreadCountY, kWaterFftThreadCountZ);
    m_buildMapsPipeline = createComputePipeline(
        shaderDirectory / "water_build_maps.comp.spv",
        1, 0, 2, 3, 0,
        kWaterComputeThreadCountX, kWaterComputeThreadCountY, kWaterComputeThreadCountZ);
}

void QuadtreeWaterMeshRenderer::createMesh()
{
    createMeshGeometry(AppConfig::Water::kMeshVertexResolution);
}

void QuadtreeWaterMeshRenderer::createMeshGeometry(std::uint32_t vertexResolution)
{
    MeshResources& resources = m_mesh;
    resources.vertexResolution = std::max(2u, vertexResolution);

    const std::uint32_t n = resources.vertexResolution;
    const std::uint32_t quadCount = n - 1;

    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<std::size_t>(n) * static_cast<std::size_t>(n));
    for (std::uint32_t y = 0; y < n; ++y)
    {
        for (std::uint32_t x = 0; x < n; ++x)
        {
            Vertex vertex{};
            const float inset = 1.0f / static_cast<float>(n - 1);
            const float usableSpan = 1.0f - (inset * 2.0f);
            const float normalizedX = static_cast<float>(x) / static_cast<float>(n - 1);
            const float normalizedY = static_cast<float>(y) / static_cast<float>(n - 1);
            vertex.localCoord[0] = inset + (normalizedX * usableSpan);
            vertex.localCoord[1] = inset + (normalizedY * usableSpan);
            vertices.push_back(vertex);
        }
    }

    std::vector<std::uint32_t> indices;
    indices.reserve(static_cast<std::size_t>(quadCount) * static_cast<std::size_t>(quadCount) * 6u);
    for (std::uint32_t y = 0; y < quadCount; ++y)
    {
        for (std::uint32_t x = 0; x < quadCount; ++x)
        {
            const std::uint32_t i0 = y * n + x;
            const std::uint32_t i1 = y * n + x + 1;
            const std::uint32_t i2 = (y + 1) * n + x;
            const std::uint32_t i3 = (y + 1) * n + x + 1;

            indices.push_back(i0);
            indices.push_back(i2);
            indices.push_back(i1);
            indices.push_back(i1);
            indices.push_back(i2);
            indices.push_back(i3);
        }
    }

    resources.indexCount = static_cast<std::uint32_t>(indices.size());

    SDL_GPUBufferCreateInfo vertexInfo{};
    vertexInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertexInfo.size = static_cast<Uint32>(sizeof(Vertex) * vertices.size());
    resources.vertexBuffer = SDL_CreateGPUBuffer(m_device, &vertexInfo);
    if (resources.vertexBuffer == nullptr)
    {
        throwSdlError("Failed to create water vertex buffer.");
    }

    SDL_GPUTransferBufferCreateInfo vertexTransferInfo{};
    vertexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    vertexTransferInfo.size = vertexInfo.size;
    resources.vertexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &vertexTransferInfo);
    if (resources.vertexTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create water vertex transfer buffer.");
    }

    void* mappedVertices = SDL_MapGPUTransferBuffer(m_device, resources.vertexTransferBuffer, false);
    std::memcpy(mappedVertices, vertices.data(), sizeof(Vertex) * vertices.size());
    SDL_UnmapGPUTransferBuffer(m_device, resources.vertexTransferBuffer);

    SDL_GPUBufferCreateInfo indexInfo{};
    indexInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    indexInfo.size = static_cast<Uint32>(sizeof(std::uint32_t) * indices.size());
    resources.indexBuffer = SDL_CreateGPUBuffer(m_device, &indexInfo);
    if (resources.indexBuffer == nullptr)
    {
        throwSdlError("Failed to create water index buffer.");
    }

    SDL_GPUTransferBufferCreateInfo indexTransferInfo{};
    indexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    indexTransferInfo.size = indexInfo.size;
    resources.indexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &indexTransferInfo);
    if (resources.indexTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create water index transfer buffer.");
    }

    void* mappedIndices = SDL_MapGPUTransferBuffer(m_device, resources.indexTransferBuffer, false);
    std::memcpy(mappedIndices, indices.data(), sizeof(std::uint32_t) * indices.size());
    SDL_UnmapGPUTransferBuffer(m_device, resources.indexTransferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);

    SDL_GPUTransferBufferLocation vertexSource{};
    vertexSource.transfer_buffer = resources.vertexTransferBuffer;
    SDL_GPUBufferRegion vertexDestination{};
    vertexDestination.buffer = resources.vertexBuffer;
    vertexDestination.size = vertexInfo.size;
    SDL_UploadToGPUBuffer(copyPass, &vertexSource, &vertexDestination, false);

    SDL_GPUTransferBufferLocation indexSource{};
    indexSource.transfer_buffer = resources.indexTransferBuffer;
    SDL_GPUBufferRegion indexDestination{};
    indexDestination.buffer = resources.indexBuffer;
    indexDestination.size = indexInfo.size;
    SDL_UploadToGPUBuffer(copyPass, &indexSource, &indexDestination, false);

    SDL_EndGPUCopyPass(copyPass);
    if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
    {
        throwSdlError("Failed to upload water mesh buffers.");
    }
}

void QuadtreeWaterMeshRenderer::destroyMesh()
{
    MeshResources& resources = m_mesh;
    if (resources.indexTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, resources.indexTransferBuffer);
        resources.indexTransferBuffer = nullptr;
    }
    if (resources.indexBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, resources.indexBuffer);
        resources.indexBuffer = nullptr;
    }
    if (resources.vertexTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, resources.vertexTransferBuffer);
        resources.vertexTransferBuffer = nullptr;
    }
    if (resources.vertexBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, resources.vertexBuffer);
        resources.vertexBuffer = nullptr;
    }
    resources.vertexResolution = 0;
    resources.indexCount = 0;
}

void QuadtreeWaterMeshRenderer::createInstanceBuffer()
{
    SDL_GPUBufferCreateInfo instanceInfo{};
    instanceInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    instanceInfo.size = static_cast<Uint32>(sizeof(InstanceData) * m_instances.instances.size());
    m_instances.instanceBuffer = SDL_CreateGPUBuffer(m_device, &instanceInfo);
    if (m_instances.instanceBuffer == nullptr)
    {
        throwSdlError("Failed to create water instance buffer.");
    }

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = instanceInfo.size;
    m_instances.instanceTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
    if (m_instances.instanceTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create water instance transfer buffer.");
    }
}

void QuadtreeWaterMeshRenderer::createWorkingBuffers()
{
    const std::uint64_t elementCount =
        static_cast<std::uint64_t>(AppConfig::Water::kCascadeResolution) *
        static_cast<std::uint64_t>(AppConfig::Water::kCascadeResolution) *
        static_cast<std::uint64_t>(AppConfig::Water::kMaxCascadeCount);
    const std::uint64_t bufferSize64 = elementCount * sizeof(glm::vec4);
    if (bufferSize64 > std::numeric_limits<Uint32>::max())
    {
        throw std::runtime_error("Water working buffer size exceeds Uint32.");
    }

    SDL_GPUBufferCreateInfo bufferInfo{};
    bufferInfo.usage =
        SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
        SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    bufferInfo.size = static_cast<Uint32>(bufferSize64);

    auto createBuffer = [this, &bufferInfo](SDL_GPUBuffer*& buffer, const char* errorMessage)
    {
        buffer = SDL_CreateGPUBuffer(m_device, &bufferInfo);
        if (buffer == nullptr)
        {
            throwSdlError(errorMessage);
        }
    };

    createBuffer(m_workingBuffers.initialSpectrum, "Failed to create water initial spectrum buffer.");
    createBuffer(m_workingBuffers.displacementSpectrumPing, "Failed to create water displacement spectrum ping buffer.");
    createBuffer(m_workingBuffers.displacementSpectrumPong, "Failed to create water displacement spectrum pong buffer.");
    createBuffer(m_workingBuffers.slopeSpectrumPing, "Failed to create water slope spectrum ping buffer.");
    createBuffer(m_workingBuffers.slopeSpectrumPong, "Failed to create water slope spectrum pong buffer.");
}

void QuadtreeWaterMeshRenderer::destroyWorkingBuffers()
{
    if (m_workingBuffers.initialSpectrum != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_workingBuffers.initialSpectrum);
        m_workingBuffers.initialSpectrum = nullptr;
    }
    if (m_workingBuffers.slopeSpectrumPong != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_workingBuffers.slopeSpectrumPong);
        m_workingBuffers.slopeSpectrumPong = nullptr;
    }
    if (m_workingBuffers.slopeSpectrumPing != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_workingBuffers.slopeSpectrumPing);
        m_workingBuffers.slopeSpectrumPing = nullptr;
    }
    if (m_workingBuffers.displacementSpectrumPong != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_workingBuffers.displacementSpectrumPong);
        m_workingBuffers.displacementSpectrumPong = nullptr;
    }
    if (m_workingBuffers.displacementSpectrumPing != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_workingBuffers.displacementSpectrumPing);
        m_workingBuffers.displacementSpectrumPing = nullptr;
    }
    m_initialSpectrumDirty = true;
}

void QuadtreeWaterMeshRenderer::createWaterTextures()
{
    SDL_GPUTextureCreateInfo textureInfo{};
    textureInfo.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    textureInfo.format = kWaterTextureFormat;
    textureInfo.width = AppConfig::Water::kCascadeResolution;
    textureInfo.height = AppConfig::Water::kCascadeResolution;
    textureInfo.layer_count_or_depth = AppConfig::Water::kMaxCascadeCount;
    textureInfo.num_levels = 1;
    textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;

    textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    m_displacementTexture = SDL_CreateGPUTexture(m_device, &textureInfo);
    if (m_displacementTexture == nullptr)
    {
        throwSdlError("Failed to create water displacement texture.");
    }

    m_slopeTexture = SDL_CreateGPUTexture(m_device, &textureInfo);
    if (m_slopeTexture == nullptr)
    {
        throwSdlError("Failed to create water slope texture.");
    }
    m_foamHistoryReadTexture = SDL_CreateGPUTexture(m_device, &textureInfo);
    if (m_foamHistoryReadTexture == nullptr)
    {
        throwSdlError("Failed to create water foam history read texture.");
    }
    m_foamHistoryWriteTexture = SDL_CreateGPUTexture(m_device, &textureInfo);
    if (m_foamHistoryWriteTexture == nullptr)
    {
        throwSdlError("Failed to create water foam history write texture.");
    }
    m_hasValidFoamHistory = false;
}

void QuadtreeWaterMeshRenderer::destroyWaterTextures()
{
    if (m_foamHistoryWriteTexture != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_foamHistoryWriteTexture);
        m_foamHistoryWriteTexture = nullptr;
    }
    if (m_foamHistoryReadTexture != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_foamHistoryReadTexture);
        m_foamHistoryReadTexture = nullptr;
    }
    if (m_slopeTexture != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_slopeTexture);
        m_slopeTexture = nullptr;
    }
    if (m_displacementTexture != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_displacementTexture);
        m_displacementTexture = nullptr;
    }
}

void QuadtreeWaterMeshRenderer::createWaterSampler()
{
    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.min_lod = 0.0f;
    samplerInfo.max_lod = 0.0f;
    m_waterSampler = SDL_CreateGPUSampler(m_device, &samplerInfo);
    if (m_waterSampler == nullptr)
    {
        throwSdlError("Failed to create water sampler.");
    }
}

void QuadtreeWaterMeshRenderer::destroyWaterSampler()
{
    if (m_waterSampler != nullptr)
    {
        SDL_ReleaseGPUSampler(m_device, m_waterSampler);
        m_waterSampler = nullptr;
    }
}

QuadtreeWaterMeshRenderer::WaterUniforms QuadtreeWaterMeshRenderer::buildWaterUniforms(
    const glm::mat4& viewProjection,
    const LightingSystem& lightingSystem,
    const SkyboxRenderer& skyboxRenderer,
    float timeSeconds) const
{
    WaterUniforms uniforms{};
    uniforms.viewProjection = viewProjection;

    const glm::dvec3 cameraWorld = m_activeCameraPosition.worldPosition();
    uniforms.cameraAndTime = glm::vec4(
        static_cast<float>(cameraWorld.x),
        static_cast<float>(cameraWorld.z),
        static_cast<float>(cameraWorld.y),
        timeSeconds);
    uniforms.waterParams = glm::vec4(
        m_settings.waterLevel,
        m_settings.globalAmplitude,
        m_settings.globalChoppiness,
        static_cast<float>(m_settings.cascadeCount));

    const glm::vec3 sunDirection = lightingSystem.sunDirection();
    uniforms.sunDirectionIntensity = glm::vec4(sunDirection, lightingSystem.sun().intensity);
    uniforms.sunColorAmbient = glm::vec4(lightingSystem.sun().color, AppConfig::Terrain::kAmbientLight);
    uniforms.debugParams = glm::vec4(m_settings.showLodTint ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    uniforms.depthEffectParams = glm::vec4(
        AppConfig::Water::kShallowDepthFadeStartMeters,
        AppConfig::Water::kShallowDepthFadeEndMeters,
        AppConfig::Water::kShorelineTintDepthMeters,
        0.0f);
    const SkyboxRenderer::SharedSkyUniforms sharedSkyUniforms = skyboxRenderer.buildSharedSkyUniforms(
        static_cast<float>(m_activeCameraPosition.localPosition().y),
        lightingSystem);
    uniforms.skyRotation = sharedSkyUniforms.skyRotation;
    uniforms.atmosphereParams = sharedSkyUniforms.atmosphereParams;
    uniforms.sunDirectionTimeOfDay = sharedSkyUniforms.sunDirectionTimeOfDay;
    uniforms.opticalParams = glm::vec4(
        AppConfig::Water::kBaseReflectance,
        AppConfig::Water::kBaseRoughness,
        AppConfig::Water::kSlopeRoughnessStrength,
        AppConfig::Water::kEnvironmentReflectionStrength);
    uniforms.refractionParams = glm::vec4(
        AppConfig::Water::kShallowRefractionMaxDepthMeters,
        AppConfig::Water::kShallowRefractionFullFadeDepthMeters,
        0.0f,
        0.0f);
    uniforms.foamParams = buildFoamGenerationParams(m_settings);
    uniforms.foamParams2 = buildFoamHistoryParams(m_settings, false);
    uniforms.foamColor = glm::vec4(
        AppConfig::Water::kCrestFoamColor,
        std::max(m_settings.crestFoamBrightness, 0.0f));
    uniforms.debugParams.y = AppConfig::Water::kSubsurfaceStrength;
    uniforms.debugParams.z = AppConfig::Water::kScatteringAnisotropy;
    uniforms.debugParams.w = AppConfig::Water::kDepthAbsorptionStrength;

    for (std::uint32_t cascadeIndex = 0; cascadeIndex < std::min(m_settings.cascadeCount, AppConfig::Water::kMaxCascadeCount); ++cascadeIndex)
    {
        const float worldSize = std::max(m_settings.cascades[cascadeIndex].worldSizeMeters, 1.0f);
        const float shallowDamping = std::max(m_settings.cascades[cascadeIndex].shallowDampingStrength, 0.0f);
        if (cascadeIndex < 4u)
        {
            (&uniforms.cascadeWorldSizesA.x)[cascadeIndex] = worldSize;
            (&uniforms.cascadeShallowDampingA.x)[cascadeIndex] = shallowDamping;
        }
        else
        {
            (&uniforms.cascadeWorldSizesB.x)[cascadeIndex - 4u] = worldSize;
            (&uniforms.cascadeShallowDampingB.x)[cascadeIndex - 4u] = shallowDamping;
        }
    }

    return uniforms;
}

QuadtreeWaterMeshRenderer::WaterSimulationUniforms QuadtreeWaterMeshRenderer::buildSimulationUniforms(
    float timeSeconds,
    std::uint32_t cascadeIndex,
    std::uint32_t stageIndex,
    std::uint32_t stageAxis) const
{
    WaterSimulationUniforms uniforms{};
    uniforms.dispatchParams = glm::uvec4(
        AppConfig::Water::kCascadeResolution,
        cascadeIndex,
        stageIndex,
        stageAxis);
    uniforms.timeAndGlobal = glm::vec4(
        timeSeconds,
        m_settings.globalAmplitude,
        m_settings.globalChoppiness,
        0.0f);
    uniforms.simulationParams = glm::vec4(
        m_settings.depthMeters,
        m_settings.lowCutoff,
        m_settings.highCutoff,
        0.0f);
    uniforms.foamParams = buildFoamGenerationParams(m_settings);
    uniforms.foamParams2 = buildFoamHistoryParams(m_settings, m_hasValidFoamHistory);

    for (std::uint32_t cascadeIndex = 0; cascadeIndex < std::min(m_settings.cascadeCount, AppConfig::Water::kMaxCascadeCount); ++cascadeIndex)
    {
        const WaterCascadeSettings& cascade = m_settings.cascades[cascadeIndex];
        const float worldSize = std::max(cascade.worldSizeMeters, 1.0f);
        const float amplitude = std::max(cascade.amplitude, 0.0f);
        const float windDirX = std::cos(cascade.windDirectionRadians);
        const float windDirZ = std::sin(cascade.windDirectionRadians);
        const float windSpeed = std::max(cascade.windSpeed, 0.0f);
        const float choppiness = std::max(cascade.choppiness, 0.0f);

        if (cascadeIndex < 4u)
        {
            (&uniforms.cascadeWorldSizesA.x)[cascadeIndex] = worldSize;
            (&uniforms.cascadeAmplitudesA.x)[cascadeIndex] = amplitude;
            (&uniforms.cascadeWindDirXA.x)[cascadeIndex] = windDirX;
            (&uniforms.cascadeWindDirZA.x)[cascadeIndex] = windDirZ;
            (&uniforms.cascadeWindSpeedsA.x)[cascadeIndex] = windSpeed;
            (&uniforms.cascadeFetchesA.x)[cascadeIndex] = cascade.fetchMeters;
            (&uniforms.cascadeSpreadBlendA.x)[cascadeIndex] = cascade.spreadBlend;
            (&uniforms.cascadeSwellA.x)[cascadeIndex] = cascade.swell;
            (&uniforms.cascadePeakEnhancementA.x)[cascadeIndex] = cascade.peakEnhancement;
            (&uniforms.cascadeShortWavesFadeA.x)[cascadeIndex] = cascade.shortWavesFade;
            (&uniforms.cascadeChoppinessA.x)[cascadeIndex] = choppiness;
        }
        else
        {
            const std::uint32_t localIndex = cascadeIndex - 4u;
            (&uniforms.cascadeWorldSizesB.x)[localIndex] = worldSize;
            (&uniforms.cascadeAmplitudesB.x)[localIndex] = amplitude;
            (&uniforms.cascadeWindDirXB.x)[localIndex] = windDirX;
            (&uniforms.cascadeWindDirZB.x)[localIndex] = windDirZ;
            (&uniforms.cascadeWindSpeedsB.x)[localIndex] = windSpeed;
            (&uniforms.cascadeFetchesB.x)[localIndex] = cascade.fetchMeters;
            (&uniforms.cascadeSpreadBlendB.x)[localIndex] = cascade.spreadBlend;
            (&uniforms.cascadeSwellB.x)[localIndex] = cascade.swell;
            (&uniforms.cascadePeakEnhancementB.x)[localIndex] = cascade.peakEnhancement;
            (&uniforms.cascadeShortWavesFadeB.x)[localIndex] = cascade.shortWavesFade;
            (&uniforms.cascadeChoppinessB.x)[localIndex] = choppiness;
        }
    }
    return uniforms;
}

void QuadtreeWaterMeshRenderer::dispatchInitializeSpectrum(
    SDL_GPUCommandBuffer* commandBuffer,
    const WaterSimulationUniforms& baseUniforms)
{
    HELLO_PROFILE_SCOPE_GROUPS("QuadtreeWaterMeshRenderer::DispatchInitializeSpectrum", ProfileScopeGroup::Renderer);
    const std::uint32_t groupCountX =
        (AppConfig::Water::kCascadeResolution + kWaterComputeThreadCountX - 1) / kWaterComputeThreadCountX;
    const std::uint32_t groupCountY =
        (AppConfig::Water::kCascadeResolution + kWaterComputeThreadCountY - 1) / kWaterComputeThreadCountY;

    WaterSimulationUniforms uniforms = baseUniforms;
    uniforms.dispatchParams.y = std::min(m_settings.cascadeCount, AppConfig::Water::kMaxCascadeCount);
    SDL_PushGPUComputeUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

    SDL_GPUStorageBufferReadWriteBinding storageBinding{};
    storageBinding.buffer = m_workingBuffers.initialSpectrum;
    storageBinding.cycle = false;

    SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(commandBuffer, nullptr, 0, &storageBinding, 1);
    if (computePass == nullptr)
    {
        throwSdlError("Failed to begin water initial spectrum compute pass.");
    }

    SDL_BindGPUComputePipeline(computePass, m_initializeSpectrumPipeline);
    SDL_DispatchGPUCompute(computePass, groupCountX, groupCountY, uniforms.dispatchParams.y);
    SDL_EndGPUComputePass(computePass);
}

void QuadtreeWaterMeshRenderer::dispatchSpectrumUpdate(
    SDL_GPUCommandBuffer* commandBuffer,
    const WaterSimulationUniforms& baseUniforms)
{
    HELLO_PROFILE_SCOPE_GROUPS("QuadtreeWaterMeshRenderer::DispatchSpectrumUpdate", ProfileScopeGroup::Renderer);
    const std::uint32_t groupCountX =
        (AppConfig::Water::kCascadeResolution + kWaterComputeThreadCountX - 1) / kWaterComputeThreadCountX;
    const std::uint32_t groupCountY =
        (AppConfig::Water::kCascadeResolution + kWaterComputeThreadCountY - 1) / kWaterComputeThreadCountY;
    WaterSimulationUniforms uniforms = baseUniforms;
    uniforms.dispatchParams.y = std::min(m_settings.cascadeCount, AppConfig::Water::kMaxCascadeCount);
    SDL_PushGPUComputeUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

    SDL_GPUStorageBufferReadWriteBinding storageBindings[2]{};
    storageBindings[0].buffer = m_workingBuffers.displacementSpectrumPing;
    storageBindings[0].cycle = false;
    storageBindings[1].buffer = m_workingBuffers.slopeSpectrumPing;
    storageBindings[1].cycle = false;

    SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(commandBuffer, nullptr, 0, storageBindings, 2);
    if (computePass == nullptr)
    {
        throwSdlError("Failed to begin water spectrum update compute pass.");
    }

    SDL_BindGPUComputePipeline(computePass, m_spectrumUpdatePipeline);
    SDL_GPUBuffer* readonlyStorageBuffers[]{ m_workingBuffers.initialSpectrum };
    SDL_BindGPUComputeStorageBuffers(computePass, 0, readonlyStorageBuffers, 1);
    SDL_DispatchGPUCompute(computePass, groupCountX, groupCountY, uniforms.dispatchParams.y);
    SDL_EndGPUComputePass(computePass);
}

void QuadtreeWaterMeshRenderer::dispatchFftStages(SDL_GPUCommandBuffer* commandBuffer, float timeSeconds)
{
    HELLO_PROFILE_SCOPE_GROUPS("QuadtreeWaterMeshRenderer::DispatchFftStages", ProfileScopeGroup::Renderer);
    const std::uint32_t activeCascadeCount = std::min(m_settings.cascadeCount, AppConfig::Water::kMaxCascadeCount);
    const std::uint32_t groupCountX = 1u;
    const std::uint32_t groupCountY = AppConfig::Water::kCascadeResolution;

    for (std::uint32_t axis = 0; axis < 2u; ++axis)
    {
        const bool horizontalPass = axis == 0u;
        const WaterSimulationUniforms uniforms = buildSimulationUniforms(timeSeconds, activeCascadeCount, 0u, axis);
        const auto dispatchStream = [&, uniforms, horizontalPass](SDL_GPUBuffer* inputBuffer, SDL_GPUBuffer* outputBuffer)
        {
            SDL_PushGPUComputeUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

            SDL_GPUStorageBufferReadWriteBinding storageBinding{};
            storageBinding.buffer = outputBuffer;
            storageBinding.cycle = false;

            SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(commandBuffer, nullptr, 0, &storageBinding, 1);
            if (computePass == nullptr)
            {
                throwSdlError("Failed to begin water FFT compute pass.");
            }

            SDL_BindGPUComputePipeline(computePass, m_fftStagePipeline);
            SDL_GPUBuffer* readonlyStorageBuffers[]{ inputBuffer };
            SDL_BindGPUComputeStorageBuffers(computePass, 0, readonlyStorageBuffers, 1);
            SDL_DispatchGPUCompute(computePass, groupCountX, groupCountY, activeCascadeCount);
            SDL_EndGPUComputePass(computePass);
        };

        dispatchStream(
            horizontalPass ? m_workingBuffers.displacementSpectrumPing : m_workingBuffers.displacementSpectrumPong,
            horizontalPass ? m_workingBuffers.displacementSpectrumPong : m_workingBuffers.displacementSpectrumPing);
        dispatchStream(
            horizontalPass ? m_workingBuffers.slopeSpectrumPing : m_workingBuffers.slopeSpectrumPong,
            horizontalPass ? m_workingBuffers.slopeSpectrumPong : m_workingBuffers.slopeSpectrumPing);
    }
}

void QuadtreeWaterMeshRenderer::dispatchBuildMaps(
    SDL_GPUCommandBuffer* commandBuffer,
    const WaterSimulationUniforms& baseUniforms)
{
    HELLO_PROFILE_SCOPE_GROUPS("QuadtreeWaterMeshRenderer::DispatchBuildMaps", ProfileScopeGroup::Renderer);
    const std::uint32_t groupCountX =
        (AppConfig::Water::kCascadeResolution + kWaterComputeThreadCountX - 1) / kWaterComputeThreadCountX;
    const std::uint32_t groupCountY =
        (AppConfig::Water::kCascadeResolution + kWaterComputeThreadCountY - 1) / kWaterComputeThreadCountY;

    for (std::uint32_t cascadeIndex = 0; cascadeIndex < std::min(m_settings.cascadeCount, AppConfig::Water::kMaxCascadeCount); ++cascadeIndex)
    {
        WaterSimulationUniforms uniforms = baseUniforms;
        uniforms.dispatchParams.y = cascadeIndex;
        SDL_PushGPUComputeUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

        SDL_GPUStorageTextureReadWriteBinding storageBindings[3]{};
        storageBindings[0].texture = m_displacementTexture;
        storageBindings[0].mip_level = 0;
        storageBindings[0].layer = cascadeIndex;
        storageBindings[0].cycle = false;
        storageBindings[1].texture = m_slopeTexture;
        storageBindings[1].mip_level = 0;
        storageBindings[1].layer = cascadeIndex;
        storageBindings[1].cycle = false;
        storageBindings[2].texture = m_foamHistoryWriteTexture;
        storageBindings[2].mip_level = 0;
        storageBindings[2].layer = cascadeIndex;
        storageBindings[2].cycle = false;

        SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(commandBuffer, storageBindings, 3, nullptr, 0);
        if (computePass == nullptr)
        {
            throwSdlError("Failed to begin water map build compute pass.");
        }

        SDL_BindGPUComputePipeline(computePass, m_buildMapsPipeline);
        const SDL_GPUTextureSamplerBinding samplerBindings[1]{
            { m_foamHistoryReadTexture, m_waterSampler },
        };
        SDL_BindGPUComputeSamplers(computePass, 0, samplerBindings, 1);
        SDL_GPUBuffer* readonlyStorageBuffers[]{
            m_workingBuffers.displacementSpectrumPing,
            m_workingBuffers.slopeSpectrumPing,
        };
        SDL_BindGPUComputeStorageBuffers(computePass, 0, readonlyStorageBuffers, 2);
        SDL_DispatchGPUCompute(computePass, groupCountX, groupCountY, 1);
        SDL_EndGPUComputePass(computePass);
    }
    std::swap(m_foamHistoryReadTexture, m_foamHistoryWriteTexture);
    m_hasValidFoamHistory = true;
}

void QuadtreeWaterMeshRenderer::destroyInstanceBuffer()
{
    if (m_instances.instanceTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_instances.instanceTransferBuffer);
        m_instances.instanceTransferBuffer = nullptr;
    }
    if (m_instances.instanceBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_instances.instanceBuffer);
        m_instances.instanceBuffer = nullptr;
    }
    m_instances.instanceCount = 0;
}
