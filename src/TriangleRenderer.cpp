#include "TriangleRenderer.hpp"

#include "PerformanceCapture.hpp"

#include <SDL3/SDL_stdinc.h>

#include <array>
#include <cstring>
#include <stdexcept>

namespace
{
constexpr std::array<TriangleRenderer::Vertex, 3> kTriangleVertices{{
    {{ 0.0f, -0.22f, 0.0f }, { 1.0f, 0.0f, 0.0f }},
    {{ 0.22f,  0.22f, 0.0f }, { 0.0f, 1.0f, 0.0f }},
    {{-0.22f,  0.22f, 0.0f }, { 0.0f, 0.0f, 1.0f }},
}};
}

void TriangleRenderer::initialize(
    SDL_GPUDevice* device,
    SDL_GPUTextureFormat colorFormat,
    SDL_GPUTextureFormat depthFormat,
    const std::filesystem::path& shaderDirectory
)
{
    initializeRendererBase(device, colorFormat, depthFormat);

    createStaticVertexResources();
    ensureInstanceCapacity(64);
    createPipeline(shaderDirectory);
}

void TriangleRenderer::shutdown()
{
    if (m_pipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
        m_pipeline = nullptr;
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

    m_instances.clear();
    m_instanceCapacity = 0;
}

void TriangleRenderer::clear()
{
    m_instances.clear();
}

void TriangleRenderer::setActiveCamera(const Position& cameraPosition)
{
    setActiveCameraPosition(cameraPosition);
}

void TriangleRenderer::addTriangle(const Position& position)
{
    const glm::vec3 localOffset = localPositionFromWorldPosition(position);
    m_instances.push_back({
        { localOffset.x, localOffset.y, localOffset.z }
    });
}

void TriangleRenderer::upload(SDL_GPUCopyPass* copyPass)
{
    HELLO_PROFILE_SCOPE("TriangleRenderer::Upload");

    if (m_instances.empty())
    {
        return;
    }

    ensureInstanceCapacity(static_cast<std::uint32_t>(m_instances.size()));

    void* mapped = SDL_MapGPUTransferBuffer(m_device, m_instanceTransferBuffer, true);
    std::memcpy(mapped, m_instances.data(), sizeof(InstanceData) * m_instances.size());
    SDL_UnmapGPUTransferBuffer(m_device, m_instanceTransferBuffer);

    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = m_instanceTransferBuffer;
    source.offset = 0;

    SDL_GPUBufferRegion destination{};
    destination.buffer = m_instanceBuffer;
    destination.offset = 0;
    destination.size = static_cast<Uint32>(sizeof(InstanceData) * m_instances.size());

    SDL_UploadToGPUBuffer(copyPass, &source, &destination, true);
}

void TriangleRenderer::render(SDL_GPURenderPass* renderPass, SDL_GPUCommandBuffer* commandBuffer, const glm::mat4& viewProjection) const
{
    HELLO_PROFILE_SCOPE("TriangleRenderer::Render");

    if (m_instances.empty())
    {
        return;
    }

    SDL_BindGPUGraphicsPipeline(renderPass, m_pipeline);

    const SDL_GPUBufferBinding bindings[2]{
        { m_vertexBuffer, 0 },
        { m_instanceBuffer, 0 }
    };
    SDL_BindGPUVertexBuffers(renderPass, 0, bindings, 2);
    SDL_PushGPUVertexUniformData(commandBuffer, 0, &viewProjection, sizeof(glm::mat4));
    SDL_DrawGPUPrimitives(renderPass, 3, static_cast<Uint32>(m_instances.size()), 0, 0);
}

void TriangleRenderer::createPipeline(const std::filesystem::path& shaderDirectory)
{
    SDL_GPUShader* vertexShader = createShader(shaderDirectory / "triangle.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 1);
    SDL_GPUShader* fragmentShader = createShader(shaderDirectory / "triangle.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 0);

    SDL_GPUVertexBufferDescription vertexBufferDescriptions[2]{};
    vertexBufferDescriptions[0].slot = 0;
    vertexBufferDescriptions[0].pitch = sizeof(Vertex);
    vertexBufferDescriptions[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vertexBufferDescriptions[1].slot = 1;
    vertexBufferDescriptions[1].pitch = sizeof(InstanceData);
    vertexBufferDescriptions[1].input_rate = SDL_GPU_VERTEXINPUTRATE_INSTANCE;
    vertexBufferDescriptions[1].instance_step_rate = 0;

    SDL_GPUVertexAttribute vertexAttributes[3]{};
    vertexAttributes[0].location = 0;
    vertexAttributes[0].buffer_slot = 0;
    vertexAttributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertexAttributes[0].offset = offsetof(Vertex, position);
    vertexAttributes[1].location = 1;
    vertexAttributes[1].buffer_slot = 0;
    vertexAttributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertexAttributes[1].offset = offsetof(Vertex, color);
    vertexAttributes[2].location = 2;
    vertexAttributes[2].buffer_slot = 1;
    vertexAttributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertexAttributes[2].offset = offsetof(InstanceData, offset);

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
    pipelineInfo.vertex_input_state.num_vertex_buffers = 2;
    pipelineInfo.vertex_input_state.vertex_buffer_descriptions = vertexBufferDescriptions;
    pipelineInfo.vertex_input_state.num_vertex_attributes = 3;
    pipelineInfo.vertex_input_state.vertex_attributes = vertexAttributes;

    m_pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
    SDL_ReleaseGPUShader(m_device, fragmentShader);
    SDL_ReleaseGPUShader(m_device, vertexShader);

    if (m_pipeline == nullptr)
    {
        throwSdlError("Failed to create triangle graphics pipeline.");
    }
}

void TriangleRenderer::createStaticVertexResources()
{
    SDL_GPUBufferCreateInfo vertexInfo{};
    vertexInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertexInfo.size = static_cast<Uint32>(sizeof(Vertex) * kTriangleVertices.size());
    m_vertexBuffer = SDL_CreateGPUBuffer(m_device, &vertexInfo);
    if (m_vertexBuffer == nullptr)
    {
        throwSdlError("Failed to create triangle vertex buffer.");
    }

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = vertexInfo.size;
    m_vertexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
    if (m_vertexTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create triangle vertex transfer buffer.");
    }

    void* mapped = SDL_MapGPUTransferBuffer(m_device, m_vertexTransferBuffer, false);
    std::memcpy(mapped, kTriangleVertices.data(), sizeof(Vertex) * kTriangleVertices.size());
    SDL_UnmapGPUTransferBuffer(m_device, m_vertexTransferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);

    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = m_vertexTransferBuffer;
    source.offset = 0;

    SDL_GPUBufferRegion destination{};
    destination.buffer = m_vertexBuffer;
    destination.offset = 0;
    destination.size = vertexInfo.size;

    SDL_UploadToGPUBuffer(copyPass, &source, &destination, false);
    SDL_EndGPUCopyPass(copyPass);

    if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
    {
        throwSdlError("Failed to upload triangle vertex data.");
    }
}

void TriangleRenderer::ensureInstanceCapacity(std::uint32_t requiredInstanceCount)
{
    if (requiredInstanceCount <= m_instanceCapacity)
    {
        return;
    }

    std::uint32_t newCapacity = std::max<std::uint32_t>(requiredInstanceCount, std::max<std::uint32_t>(m_instanceCapacity * 2, 64));

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

    SDL_GPUBufferCreateInfo bufferInfo{};
    bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bufferInfo.size = static_cast<Uint32>(sizeof(InstanceData) * newCapacity);
    m_instanceBuffer = SDL_CreateGPUBuffer(m_device, &bufferInfo);
    if (m_instanceBuffer == nullptr)
    {
        throwSdlError("Failed to create triangle instance buffer.");
    }

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = bufferInfo.size;
    m_instanceTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
    if (m_instanceTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create triangle instance transfer buffer.");
    }

    m_instanceCapacity = newCapacity;
}
