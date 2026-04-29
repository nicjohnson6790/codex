#include "QuadtreeWaterMeshRenderer.hpp"

#include "AppConfig.hpp"
#include "PerformanceCapture.hpp"

#include <SDL3/SDL_stdinc.h>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace
{
constexpr std::uint32_t kWaterComputeThreadCountX = 16;
constexpr std::uint32_t kWaterComputeThreadCountY = 16;
constexpr std::uint32_t kWaterComputeThreadCountZ = 1;
constexpr SDL_GPUTextureFormat kWaterTextureFormat = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
constexpr std::uint32_t kFftStageCount = 8;
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
    createWaterTextures();
    createWaterSampler();
    createPipeline(shaderDirectory);
    createWaterComputePipelines(shaderDirectory);
}

void QuadtreeWaterMeshRenderer::shutdown()
{
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
        0.0f,
        0.0f);
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
        m_spectrumUpdatePipeline == nullptr ||
        m_fftStagePipeline == nullptr ||
        m_buildMapsPipeline == nullptr ||
        m_spectrumPing == nullptr ||
        m_spectrumPong == nullptr ||
        m_slopeSpectrumPing == nullptr ||
        m_slopeSpectrumPong == nullptr ||
        m_displacementTexture == nullptr ||
        m_slopeTexture == nullptr ||
        m_waterSampler == nullptr)
    {
        return;
    }

    const WaterSimulationUniforms buildUniforms = buildSimulationUniforms(timeSeconds, 0u, 0u, 0u);
    dispatchSpectrumUpdate(commandBuffer, buildUniforms);
    dispatchFftStages(commandBuffer, timeSeconds);
    dispatchBuildMaps(commandBuffer, buildUniforms);
}

void QuadtreeWaterMeshRenderer::render(
    SDL_GPURenderPass* renderPass,
    SDL_GPUCommandBuffer* commandBuffer,
    const glm::mat4& viewProjection,
    const LightingSystem& lightingSystem,
    float timeSeconds) const
{
    HELLO_PROFILE_SCOPE_GROUPS("QuadtreeWaterMeshRenderer::Render", ProfileScopeGroup::Renderer);

    if (!m_settings.enabled ||
        totalInstanceCount() == 0 ||
        m_pipeline == nullptr ||
        m_displacementTexture == nullptr ||
        m_slopeTexture == nullptr ||
        m_waterSampler == nullptr)
    {
        return;
    }

    SDL_BindGPUGraphicsPipeline(renderPass, m_pipeline);

    const WaterUniforms uniforms = buildWaterUniforms(viewProjection, lightingSystem, timeSeconds);
    SDL_PushGPUVertexUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));
    SDL_PushGPUFragmentUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

    const SDL_GPUTextureSamplerBinding vertexSamplerBindings[1]{
        { m_displacementTexture, m_waterSampler },
    };
    SDL_BindGPUVertexSamplers(renderPass, 0, vertexSamplerBindings, 1);

    const SDL_GPUTextureSamplerBinding fragmentSamplerBindings[2]{
        { m_displacementTexture, m_waterSampler },
        { m_slopeTexture, m_waterSampler },
    };
    SDL_BindGPUFragmentSamplers(renderPass, 0, fragmentSamplerBindings, 2);

    if (m_mesh.vertexBuffer == nullptr || m_mesh.indexBuffer == nullptr || m_instances.instanceCount == 0)
    {
        return;
    }

    const SDL_GPUBufferBinding vertexBinding{ m_mesh.vertexBuffer, 0 };
    SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

    const SDL_GPUBufferBinding indexBinding{ m_mesh.indexBuffer, 0 };
    SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    SDL_GPUBuffer* storageBuffers[]{ m_instances.instanceBuffer };
    SDL_BindGPUVertexStorageBuffers(renderPass, 0, storageBuffers, 1);

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
    SDL_GPUShader* vertexShader = createShader(shaderDirectory / "water_mesh.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 1, 1, 1);
    SDL_GPUShader* fragmentShader = createShader(shaderDirectory / "water_mesh.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, 2);

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
                                           std::uint32_t readwriteStorageTextureCount) -> SDL_GPUComputePipeline*
    {
        const std::vector<std::uint8_t> bytes = readShaderCode(path);

        SDL_GPUComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.code_size = bytes.size();
        pipelineInfo.code = bytes.data();
        pipelineInfo.entrypoint = "main";
        pipelineInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
        pipelineInfo.num_samplers = samplerCount;
        pipelineInfo.num_readonly_storage_textures = readonlyStorageTextureCount;
        pipelineInfo.num_readonly_storage_buffers = 0;
        pipelineInfo.num_readwrite_storage_textures = readwriteStorageTextureCount;
        pipelineInfo.num_readwrite_storage_buffers = 0;
        pipelineInfo.num_uniform_buffers = 1;
        pipelineInfo.threadcount_x = kWaterComputeThreadCountX;
        pipelineInfo.threadcount_y = kWaterComputeThreadCountY;
        pipelineInfo.threadcount_z = kWaterComputeThreadCountZ;

        SDL_GPUComputePipeline* pipeline = SDL_CreateGPUComputePipeline(m_device, &pipelineInfo);
        if (pipeline == nullptr)
        {
            throwSdlError(("Failed to create water compute pipeline: " + path.string()).c_str());
        }

        return pipeline;
    };

    m_spectrumUpdatePipeline = createComputePipeline(shaderDirectory / "water_spectrum_update.comp.spv", 0, 0, 2);
    m_fftStagePipeline = createComputePipeline(shaderDirectory / "water_fft_stage.comp.spv", 1, 0, 1);
    m_buildMapsPipeline = createComputePipeline(shaderDirectory / "water_build_maps.comp.spv", 2, 0, 2);
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

    textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    m_spectrumPing = SDL_CreateGPUTexture(m_device, &textureInfo);
    if (m_spectrumPing == nullptr)
    {
        throwSdlError("Failed to create water spectrum ping texture.");
    }

    m_spectrumPong = SDL_CreateGPUTexture(m_device, &textureInfo);
    if (m_spectrumPong == nullptr)
    {
        throwSdlError("Failed to create water spectrum pong texture.");
    }

    m_slopeSpectrumPing = SDL_CreateGPUTexture(m_device, &textureInfo);
    if (m_slopeSpectrumPing == nullptr)
    {
        throwSdlError("Failed to create water slope spectrum ping texture.");
    }

    m_slopeSpectrumPong = SDL_CreateGPUTexture(m_device, &textureInfo);
    if (m_slopeSpectrumPong == nullptr)
    {
        throwSdlError("Failed to create water slope spectrum pong texture.");
    }

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
}

void QuadtreeWaterMeshRenderer::destroyWaterTextures()
{
    if (m_slopeTexture != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_slopeTexture);
        m_slopeTexture = nullptr;
    }
    if (m_slopeSpectrumPong != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_slopeSpectrumPong);
        m_slopeSpectrumPong = nullptr;
    }
    if (m_slopeSpectrumPing != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_slopeSpectrumPing);
        m_slopeSpectrumPing = nullptr;
    }
    if (m_displacementTexture != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_displacementTexture);
        m_displacementTexture = nullptr;
    }
    if (m_spectrumPong != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_spectrumPong);
        m_spectrumPong = nullptr;
    }
    if (m_spectrumPing != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_spectrumPing);
        m_spectrumPing = nullptr;
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

    for (std::uint32_t cascadeIndex = 0; cascadeIndex < std::min(m_settings.cascadeCount, AppConfig::Water::kMaxCascadeCount); ++cascadeIndex)
    {
        const float worldSize = std::max(m_settings.cascades[cascadeIndex].worldSizeMeters, 1.0f);
        if (cascadeIndex < 4u)
        {
            (&uniforms.cascadeWorldSizesA.x)[cascadeIndex] = worldSize;
        }
        else
        {
            (&uniforms.cascadeWorldSizesB.x)[cascadeIndex - 4u] = worldSize;
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
            (&uniforms.cascadeChoppinessB.x)[localIndex] = choppiness;
        }
    }

    return uniforms;
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

    for (std::uint32_t cascadeIndex = 0; cascadeIndex < std::min(m_settings.cascadeCount, AppConfig::Water::kMaxCascadeCount); ++cascadeIndex)
    {
        WaterSimulationUniforms uniforms = baseUniforms;
        uniforms.dispatchParams.y = cascadeIndex;
        SDL_PushGPUComputeUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

        SDL_GPUStorageTextureReadWriteBinding storageBindings[2]{};
        storageBindings[0].texture = m_spectrumPing;
        storageBindings[0].mip_level = 0;
        storageBindings[0].layer = cascadeIndex;
        storageBindings[0].cycle = false;
        storageBindings[1].texture = m_slopeSpectrumPing;
        storageBindings[1].mip_level = 0;
        storageBindings[1].layer = cascadeIndex;
        storageBindings[1].cycle = false;

        SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(commandBuffer, storageBindings, 2, nullptr, 0);
        if (computePass == nullptr)
        {
            throwSdlError("Failed to begin water spectrum update compute pass.");
        }

        SDL_BindGPUComputePipeline(computePass, m_spectrumUpdatePipeline);
        SDL_DispatchGPUCompute(computePass, groupCountX, groupCountY, 1);
        SDL_EndGPUComputePass(computePass);
    }
}

void QuadtreeWaterMeshRenderer::dispatchFftStages(SDL_GPUCommandBuffer* commandBuffer, float timeSeconds)
{
    HELLO_PROFILE_SCOPE_GROUPS("QuadtreeWaterMeshRenderer::DispatchFftStages", ProfileScopeGroup::Renderer);
    const std::uint32_t groupCountX =
        (AppConfig::Water::kCascadeResolution + kWaterComputeThreadCountX - 1) / kWaterComputeThreadCountX;
    const std::uint32_t groupCountY =
        (AppConfig::Water::kCascadeResolution + kWaterComputeThreadCountY - 1) / kWaterComputeThreadCountY;

    for (std::uint32_t axis = 0; axis < 2u; ++axis)
    {
        for (std::uint32_t stageIndex = 0; stageIndex < kFftStageCount; ++stageIndex)
        {
            const bool evenStage = ((axis * kFftStageCount) + stageIndex) % 2u == 0u;
            for (std::uint32_t cascadeIndex = 0; cascadeIndex < std::min(m_settings.cascadeCount, AppConfig::Water::kMaxCascadeCount); ++cascadeIndex)
            {
                const WaterSimulationUniforms uniforms = buildSimulationUniforms(timeSeconds, cascadeIndex, stageIndex, axis);
                const auto dispatchStream = [&, cascadeIndex, uniforms](SDL_GPUTexture* inputTexture, SDL_GPUTexture* outputTexture)
                {
                    SDL_PushGPUComputeUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

                    SDL_GPUStorageTextureReadWriteBinding storageBinding{};
                    storageBinding.texture = outputTexture;
                    storageBinding.mip_level = 0;
                    storageBinding.layer = cascadeIndex;
                    storageBinding.cycle = false;

                    SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(commandBuffer, &storageBinding, 1, nullptr, 0);
                    if (computePass == nullptr)
                    {
                        throwSdlError("Failed to begin water FFT compute pass.");
                    }

                    SDL_BindGPUComputePipeline(computePass, m_fftStagePipeline);
                    const SDL_GPUTextureSamplerBinding samplerBinding{ inputTexture, m_waterSampler };
                    SDL_BindGPUComputeSamplers(computePass, 0, &samplerBinding, 1);
                    SDL_DispatchGPUCompute(computePass, groupCountX, groupCountY, 1);
                    SDL_EndGPUComputePass(computePass);
                };

                dispatchStream(
                    evenStage ? m_spectrumPing : m_spectrumPong,
                    evenStage ? m_spectrumPong : m_spectrumPing);
                dispatchStream(
                    evenStage ? m_slopeSpectrumPing : m_slopeSpectrumPong,
                    evenStage ? m_slopeSpectrumPong : m_slopeSpectrumPing);
            }
        }
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

        SDL_GPUStorageTextureReadWriteBinding storageBindings[2]{};
        storageBindings[0].texture = m_displacementTexture;
        storageBindings[0].mip_level = 0;
        storageBindings[0].layer = cascadeIndex;
        storageBindings[0].cycle = false;
        storageBindings[1].texture = m_slopeTexture;
        storageBindings[1].mip_level = 0;
        storageBindings[1].layer = cascadeIndex;
        storageBindings[1].cycle = false;

        SDL_GPUComputePass* computePass = SDL_BeginGPUComputePass(commandBuffer, storageBindings, 2, nullptr, 0);
        if (computePass == nullptr)
        {
            throwSdlError("Failed to begin water map build compute pass.");
        }

        SDL_BindGPUComputePipeline(computePass, m_buildMapsPipeline);
        const SDL_GPUTextureSamplerBinding samplerBindings[2]{
            { m_spectrumPing, m_waterSampler },
            { m_slopeSpectrumPing, m_waterSampler },
        };
        SDL_BindGPUComputeSamplers(computePass, 0, samplerBindings, 2);
        SDL_DispatchGPUCompute(computePass, groupCountX, groupCountY, 1);
        SDL_EndGPUComputePass(computePass);
    }
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
