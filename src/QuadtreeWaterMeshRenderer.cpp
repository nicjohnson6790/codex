#include "QuadtreeWaterMeshRenderer.hpp"

#include "AppConfig.hpp"
#include "PerformanceCapture.hpp"

#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

void QuadtreeWaterMeshRenderer::initialize(
    SDL_GPUDevice* device,
    SDL_GPUTextureFormat colorFormat,
    SDL_GPUTextureFormat depthFormat,
    const std::filesystem::path& shaderDirectory)
{
    initializeRendererBase(device, colorFormat, depthFormat);
    m_settings = makeDefaultWaterSettings();
    createInstanceBuffers();
    createMeshLods();
    createPipeline(shaderDirectory);
}

void QuadtreeWaterMeshRenderer::shutdown()
{
    if (m_pipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
        m_pipeline = nullptr;
    }

    destroyMeshLods();
    destroyInstanceBuffers();
    clear();
}

void QuadtreeWaterMeshRenderer::clear()
{
    for (InstanceLodResources& lodResources : m_instanceLods)
    {
        lodResources.instanceCount = 0;
    }
}

void QuadtreeWaterMeshRenderer::setActiveCamera(const Position& cameraPosition)
{
    setActiveCameraPosition(cameraPosition);
}

void QuadtreeWaterMeshRenderer::setSettings(const WaterSettings& settings)
{
    m_settings = settings;
}

void QuadtreeWaterMeshRenderer::rebuildMeshLods()
{
    if (m_device == nullptr)
    {
        return;
    }

    if (!SDL_WaitForGPUIdle(m_device))
    {
        throwSdlError("Failed to wait for GPU idle before rebuilding water meshes.");
    }

    destroyMeshLods();
    createMeshLods();
}

void QuadtreeWaterMeshRenderer::addLeaf(
    const WorldGridQuadtreeLeafId& leafId,
    const Position& leafOrigin,
    double leafSizeMeters,
    std::uint8_t quadtreeLodHint,
    std::uint8_t waterMeshLod,
    std::uint32_t bandMask)
{
    (void)leafId;

    if (waterMeshLod >= AppConfig::Water::kMeshLodCount)
    {
        waterMeshLod = static_cast<std::uint8_t>(AppConfig::Water::kMeshLodCount - 1);
    }

    InstanceLodResources& lodResources = m_instanceLods[waterMeshLod];
    if (lodResources.instanceCount >= AppConfig::Water::kMaxWaterInstancesPerLod)
    {
        return;
    }

    const glm::vec3 localOrigin = localPositionFromWorldPosition(leafOrigin);

    InstanceData& instance = lodResources.instances[lodResources.instanceCount++];
    instance.position[0] = localOrigin.x;
    instance.position[1] = localOrigin.y;
    instance.position[2] = localOrigin.z;
    instance.packedMetadata = packMetadata(quadtreeLodHint, waterMeshLod, bandMask);
    instance.leafParams = glm::vec4(
        static_cast<float>(leafSizeMeters),
        m_settings.waterLevel,
        0.0f,
        0.0f);
}

void QuadtreeWaterMeshRenderer::upload(SDL_GPUCopyPass* copyPass)
{
    HELLO_PROFILE_SCOPE("QuadtreeWaterMeshRenderer::Upload");

    for (std::uint32_t lodIndex = 0; lodIndex < AppConfig::Water::kMeshLodCount; ++lodIndex)
    {
        InstanceLodResources& lodResources = m_instanceLods[lodIndex];
        if (lodResources.instanceCount == 0)
        {
            continue;
        }

        void* mappedInstances = SDL_MapGPUTransferBuffer(m_device, lodResources.instanceTransferBuffer, true);
        std::memcpy(
            mappedInstances,
            lodResources.instances.data(),
            sizeof(InstanceData) * lodResources.instanceCount);
        SDL_UnmapGPUTransferBuffer(m_device, lodResources.instanceTransferBuffer);

        SDL_GPUTransferBufferLocation source{};
        source.transfer_buffer = lodResources.instanceTransferBuffer;

        SDL_GPUBufferRegion destination{};
        destination.buffer = lodResources.instanceBuffer;
        destination.size = static_cast<Uint32>(sizeof(InstanceData) * lodResources.instanceCount);
        SDL_UploadToGPUBuffer(copyPass, &source, &destination, true);
    }
}

void QuadtreeWaterMeshRenderer::dispatchWaterSimulation(
    SDL_GPUCommandBuffer* commandBuffer,
    float timeSeconds,
    std::uint64_t frameIndex)
{
    (void)commandBuffer;
    (void)timeSeconds;
    (void)frameIndex;
}

void QuadtreeWaterMeshRenderer::render(
    SDL_GPURenderPass* renderPass,
    SDL_GPUCommandBuffer* commandBuffer,
    const glm::mat4& viewProjection,
    const LightingSystem& lightingSystem,
    float timeSeconds) const
{
    HELLO_PROFILE_SCOPE("QuadtreeWaterMeshRenderer::Render");

    if (!m_settings.enabled || totalInstanceCount() == 0)
    {
        return;
    }

    SDL_BindGPUGraphicsPipeline(renderPass, m_pipeline);

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
    SDL_PushGPUVertexUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

    for (std::uint32_t lodIndex = 0; lodIndex < AppConfig::Water::kMeshLodCount; ++lodIndex)
    {
        const MeshLodResources& meshResources = m_meshLods[lodIndex];
        const InstanceLodResources& instanceResources = m_instanceLods[lodIndex];
        if (instanceResources.instanceCount == 0 || meshResources.vertexBuffer == nullptr || meshResources.indexBuffer == nullptr)
        {
            continue;
        }

        const SDL_GPUBufferBinding vertexBinding{ meshResources.vertexBuffer, 0 };
        SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

        const SDL_GPUBufferBinding indexBinding{ meshResources.indexBuffer, 0 };
        SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        SDL_GPUBuffer* storageBuffers[]{ instanceResources.instanceBuffer };
        SDL_BindGPUVertexStorageBuffers(renderPass, 0, storageBuffers, 1);

        SDL_DrawGPUIndexedPrimitives(
            renderPass,
            meshResources.indexCount,
            instanceResources.instanceCount,
            0,
            0,
            0);
    }
}

std::uint32_t QuadtreeWaterMeshRenderer::instanceCountForLod(std::uint32_t lodIndex) const
{
    if (lodIndex >= AppConfig::Water::kMeshLodCount)
    {
        return 0;
    }

    return m_instanceLods[lodIndex].instanceCount;
}

std::uint32_t QuadtreeWaterMeshRenderer::totalInstanceCount() const
{
    std::uint32_t total = 0;
    for (const InstanceLodResources& lodResources : m_instanceLods)
    {
        total += lodResources.instanceCount;
    }
    return total;
}

std::uint32_t QuadtreeWaterMeshRenderer::packMetadata(
    std::uint8_t quadtreeLodHint,
    std::uint8_t waterMeshLod,
    std::uint32_t bandMask)
{
    return
        (static_cast<std::uint32_t>(quadtreeLodHint) & 0xFFu) |
        ((static_cast<std::uint32_t>(waterMeshLod) & 0xFFu) << 8) |
        ((bandMask & 0xFFFFu) << 16);
}

void QuadtreeWaterMeshRenderer::createPipeline(const std::filesystem::path& shaderDirectory)
{
    SDL_GPUShader* vertexShader = createShader(shaderDirectory / "water_mesh.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 1, 1);
    SDL_GPUShader* fragmentShader = createShader(shaderDirectory / "water_mesh.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);

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

void QuadtreeWaterMeshRenderer::createMeshLods()
{
    for (std::uint32_t lodIndex = 0; lodIndex < AppConfig::Water::kMeshLodCount; ++lodIndex)
    {
        createMeshLod(lodIndex, m_settings.meshLods[lodIndex].vertexResolution);
    }
}

void QuadtreeWaterMeshRenderer::createMeshLod(std::uint32_t lodIndex, std::uint32_t vertexResolution)
{
    MeshLodResources& resources = m_meshLods[lodIndex];
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

void QuadtreeWaterMeshRenderer::destroyMeshLods()
{
    for (MeshLodResources& resources : m_meshLods)
    {
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
}

void QuadtreeWaterMeshRenderer::createInstanceBuffers()
{
    for (InstanceLodResources& resources : m_instanceLods)
    {
        SDL_GPUBufferCreateInfo instanceInfo{};
        instanceInfo.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        instanceInfo.size = static_cast<Uint32>(sizeof(InstanceData) * resources.instances.size());
        resources.instanceBuffer = SDL_CreateGPUBuffer(m_device, &instanceInfo);
        if (resources.instanceBuffer == nullptr)
        {
            throwSdlError("Failed to create water instance buffer.");
        }

        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = instanceInfo.size;
        resources.instanceTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
        if (resources.instanceTransferBuffer == nullptr)
        {
            throwSdlError("Failed to create water instance transfer buffer.");
        }
    }
}

void QuadtreeWaterMeshRenderer::destroyInstanceBuffers()
{
    for (InstanceLodResources& resources : m_instanceLods)
    {
        if (resources.instanceTransferBuffer != nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(m_device, resources.instanceTransferBuffer);
            resources.instanceTransferBuffer = nullptr;
        }
        if (resources.instanceBuffer != nullptr)
        {
            SDL_ReleaseGPUBuffer(m_device, resources.instanceBuffer);
            resources.instanceBuffer = nullptr;
        }
        resources.instanceCount = 0;
    }
}
