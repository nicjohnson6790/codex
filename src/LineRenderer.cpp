#include "LineRenderer.hpp"

#include "PerformanceCapture.hpp"

#include <cstring>
#include <stdexcept>

void LineRenderer::initialize(
    SDL_GPUDevice* device,
    SDL_GPUTextureFormat colorFormat,
    SDL_GPUTextureFormat depthFormat,
    const std::filesystem::path& shaderDirectory
)
{
    initializeRendererBase(device, colorFormat, depthFormat);

    ensureVertexCapacity(128);
    createPipeline(shaderDirectory);
}

void LineRenderer::shutdown()
{
    if (m_pipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
        m_pipeline = nullptr;
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

    m_vertices.clear();
    m_vertexCapacity = 0;
}

void LineRenderer::clear()
{
    m_vertices.clear();
}

void LineRenderer::setActiveCamera(const Position& cameraPosition)
{
    setActiveCameraPosition(cameraPosition);
}

void LineRenderer::addLine(const Position& start, const Position& stop, const glm::vec3& color)
{
    addLine(start, stop, color, color);
}

void LineRenderer::addLine(const Position& start, const Position& stop, const glm::vec3& startColor, const glm::vec3& stopColor)
{
    const glm::vec3 localStart = localPositionFromWorldPosition(start);
    const glm::vec3 localStop = localPositionFromWorldPosition(stop);

    m_vertices.push_back({
        { localStart.x, localStart.y, localStart.z },
        { startColor.x, startColor.y, startColor.z },
    });
    m_vertices.push_back({
        { localStop.x, localStop.y, localStop.z },
        { stopColor.x, stopColor.y, stopColor.z },
    });
}

void LineRenderer::upload(SDL_GPUCopyPass* copyPass)
{
    HELLO_PROFILE_SCOPE("LineRenderer::Upload");

    if (m_vertices.empty())
    {
        return;
    }

    ensureVertexCapacity(static_cast<std::uint32_t>(m_vertices.size()));

    void* mapped = SDL_MapGPUTransferBuffer(m_device, m_vertexTransferBuffer, true);
    std::memcpy(mapped, m_vertices.data(), sizeof(Vertex) * m_vertices.size());
    SDL_UnmapGPUTransferBuffer(m_device, m_vertexTransferBuffer);

    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = m_vertexTransferBuffer;
    source.offset = 0;

    SDL_GPUBufferRegion destination{};
    destination.buffer = m_vertexBuffer;
    destination.offset = 0;
    destination.size = static_cast<Uint32>(sizeof(Vertex) * m_vertices.size());

    SDL_UploadToGPUBuffer(copyPass, &source, &destination, true);
}

void LineRenderer::render(SDL_GPURenderPass* renderPass, SDL_GPUCommandBuffer* commandBuffer, const glm::mat4& viewProjection) const
{
    HELLO_PROFILE_SCOPE("LineRenderer::Render");

    if (m_vertices.empty())
    {
        return;
    }

    SDL_BindGPUGraphicsPipeline(renderPass, m_pipeline);

    const SDL_GPUBufferBinding vertexBinding{ m_vertexBuffer, 0 };
    SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);
    SDL_PushGPUVertexUniformData(commandBuffer, 0, &viewProjection, sizeof(glm::mat4));
    SDL_DrawGPUPrimitives(renderPass, static_cast<Uint32>(m_vertices.size()), 1, 0, 0);
}

void LineRenderer::createPipeline(const std::filesystem::path& shaderDirectory)
{
    SDL_GPUShader* vertexShader = createShader(shaderDirectory / "line.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 1);
    SDL_GPUShader* fragmentShader = createShader(shaderDirectory / "line.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 0);

    SDL_GPUVertexBufferDescription vertexBufferDescription{};
    vertexBufferDescription.slot = 0;
    vertexBufferDescription.pitch = sizeof(Vertex);
    vertexBufferDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute vertexAttributes[2]{};
    vertexAttributes[0].location = 0;
    vertexAttributes[0].buffer_slot = 0;
    vertexAttributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertexAttributes[0].offset = offsetof(Vertex, position);
    vertexAttributes[1].location = 1;
    vertexAttributes[1].buffer_slot = 0;
    vertexAttributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertexAttributes[1].offset = offsetof(Vertex, color);

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
    pipelineInfo.vertex_input_state.num_vertex_attributes = 2;
    pipelineInfo.vertex_input_state.vertex_attributes = vertexAttributes;

    m_pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
    SDL_ReleaseGPUShader(m_device, fragmentShader);
    SDL_ReleaseGPUShader(m_device, vertexShader);

    if (m_pipeline == nullptr)
    {
        throwSdlError("Failed to create line graphics pipeline.");
    }
}

void LineRenderer::ensureVertexCapacity(std::uint32_t requiredVertexCount)
{
    if (requiredVertexCount <= m_vertexCapacity)
    {
        return;
    }

    const std::uint32_t newCapacity = std::max<std::uint32_t>(requiredVertexCount, std::max<std::uint32_t>(m_vertexCapacity * 2, 128));

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

    SDL_GPUBufferCreateInfo bufferInfo{};
    bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bufferInfo.size = static_cast<Uint32>(sizeof(Vertex) * newCapacity);
    m_vertexBuffer = SDL_CreateGPUBuffer(m_device, &bufferInfo);
    if (m_vertexBuffer == nullptr)
    {
        throwSdlError("Failed to create line vertex buffer.");
    }

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = bufferInfo.size;
    m_vertexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
    if (m_vertexTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create line transfer buffer.");
    }

    m_vertexCapacity = newCapacity;
}
