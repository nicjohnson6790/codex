#include "WorldTextRenderer.hpp"

#include "PerformanceCapture.hpp"
#include "assets/RuntimeAssetReader.hpp"

#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace
{
std::filesystem::path executableRelativePath(const std::filesystem::path& relativePath)
{
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr)
    {
        throw std::runtime_error(std::string("Failed to resolve executable base path: ") + SDL_GetError());
    }

    return std::filesystem::path(basePath) / relativePath;
}

SDL_GPUTextureFormat textureFormatFromRuntimeFormat(RuntimeAssets::TextureFormat format)
{
    switch (format)
    {
    case RuntimeAssets::TextureFormat::RGBA8_UNORM:
        return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    case RuntimeAssets::TextureFormat::RGBA8_SRGB:
        return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    case RuntimeAssets::TextureFormat::BC3_RGBA_UNORM:
        return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
    case RuntimeAssets::TextureFormat::BC3_RGBA_SRGB:
        return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM_SRGB;
    case RuntimeAssets::TextureFormat::BC5_RG_UNORM:
        return SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM;
    }
    throw std::runtime_error("Unsupported runtime texture format.");
}
}

void WorldTextRenderer::initialize(
    SDL_GPUDevice* device,
    SDL_GPUTextureFormat colorFormat,
    SDL_GPUTextureFormat depthFormat,
    const std::filesystem::path& shaderDirectory
)
{
    initializeRendererBase(device, colorFormat, depthFormat);

    m_groupBuffer = BufferPair{ nullptr, nullptr, 0, sizeof(GroupGpu), SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, "world text group" };
    m_glyphInstanceBuffer = BufferPair{ nullptr, nullptr, 0, sizeof(GlyphInstanceGpu), SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, "world text glyph instance" };
    m_glyphMetricBuffer = BufferPair{ nullptr, nullptr, 0, sizeof(GlyphMetricGpu), SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, "world text glyph metric" };
    m_styleBuffer = BufferPair{ nullptr, nullptr, 0, sizeof(StyleGpu), SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, "world text style" };
    m_indirectBuffer = BufferPair{ nullptr, nullptr, 0, sizeof(SDL_GPUIndexedIndirectDrawCommand), SDL_GPU_BUFFERUSAGE_INDIRECT, "world text indirect" };

    loadFontAssets();
    createSampler();
    createQuadBuffers();
    ensureBufferPairCapacity(&m_glyphMetricBuffer, static_cast<std::uint32_t>(m_glyphMetrics.size()));
    createPipeline(shaderDirectory);
}

void WorldTextRenderer::shutdown()
{
    if (m_pipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
        m_pipeline = nullptr;
    }

    if (m_quadVertexTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_quadVertexTransferBuffer);
        m_quadVertexTransferBuffer = nullptr;
    }
    if (m_quadVertexBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_quadVertexBuffer);
        m_quadVertexBuffer = nullptr;
    }
    if (m_quadIndexTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_quadIndexTransferBuffer);
        m_quadIndexTransferBuffer = nullptr;
    }
    if (m_quadIndexBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_quadIndexBuffer);
        m_quadIndexBuffer = nullptr;
    }

    destroyBufferPair(&m_groupBuffer);
    destroyBufferPair(&m_glyphInstanceBuffer);
    destroyBufferPair(&m_glyphMetricBuffer);
    destroyBufferPair(&m_styleBuffer);
    destroyBufferPair(&m_indirectBuffer);
    destroyFontResources();

    m_groups.clear();
    m_glyphInstances.clear();
    m_glyphMetrics.clear();
    m_styles.clear();
    m_drawCommands.clear();
}

void WorldTextRenderer::clear()
{
    m_groups.clear();
    m_glyphInstances.clear();
    m_styles.clear();
    m_drawCommands.clear();
}

void WorldTextRenderer::setActiveCamera(const Position& cameraPosition, const glm::vec3& cameraForward, const glm::vec3& cameraUp)
{
    setActiveCameraPosition(cameraPosition);

    glm::vec3 forward = cameraForward;
    if (glm::length(forward) <= 0.0001f)
    {
        forward = { 0.0f, 0.0f, -1.0f };
    }
    else
    {
        forward = glm::normalize(forward);
    }

    glm::vec3 right = glm::cross(forward, cameraUp);
    if (glm::length(right) <= 0.0001f)
    {
        right = { 1.0f, 0.0f, 0.0f };
    }
    right = glm::normalize(right);

    glm::vec3 up = glm::cross(right, forward);
    if (glm::length(up) <= 0.0001f)
    {
        up = { 0.0f, 1.0f, 0.0f };
    }

    m_billboardRight = right;
    m_billboardUp = glm::normalize(up);
}

void WorldTextRenderer::addLineBaseline(const Position& baseline, std::string_view text, const Style& style)
{
    addLineBaseline(baseline, m_billboardRight, m_billboardUp * m_defaultBillboardSizeMeters, text, HorizontalJustify::Left, style);
}

void WorldTextRenderer::addLineBaseline(
    const Position& baseline,
    const glm::vec3& baselineDirection,
    const glm::vec3& upDirection,
    std::string_view text,
    HorizontalJustify justify,
    const Style& style)
{
    addTextGroupLocal(
        localPositionFromWorldPosition(baseline),
        baselineDirection,
        upDirection,
        explicitSizeFromUpVector(upDirection),
        text,
        justify,
        false,
        style);
}

void WorldTextRenderer::addLineCentered(const Position& center, std::string_view text, const Style& style)
{
    addLineCentered(center, m_billboardRight, m_billboardUp * m_defaultBillboardSizeMeters, text, style);
}

void WorldTextRenderer::addLineCentered(
    const Position& center,
    const glm::vec3& baselineDirection,
    const glm::vec3& upDirection,
    std::string_view text,
    const Style& style)
{
    addTextGroupLocal(
        localPositionFromWorldPosition(center),
        baselineDirection,
        upDirection,
        explicitSizeFromUpVector(upDirection),
        text,
        HorizontalJustify::Center,
        true,
        style);
}

void WorldTextRenderer::addMultilineBaseline(
    const Position& firstBaseline,
    std::string_view text,
    HorizontalJustify justify,
    const Style& style)
{
    addMultilineBaseline(firstBaseline, m_billboardRight, m_billboardUp * m_defaultBillboardSizeMeters, text, justify, style);
}

void WorldTextRenderer::addMultilineBaseline(
    const Position& firstBaseline,
    const glm::vec3& baselineDirection,
    const glm::vec3& upDirection,
    std::string_view text,
    HorizontalJustify justify,
    const Style& style)
{
    addTextGroupLocal(
        localPositionFromWorldPosition(firstBaseline),
        baselineDirection,
        upDirection,
        explicitSizeFromUpVector(upDirection),
        text,
        justify,
        false,
        style);
}

void WorldTextRenderer::addMultilineCentered(
    const Position& center,
    std::string_view text,
    HorizontalJustify justify,
    const Style& style)
{
    addMultilineCentered(center, m_billboardRight, m_billboardUp * m_defaultBillboardSizeMeters, text, justify, style);
}

void WorldTextRenderer::addMultilineCentered(
    const Position& center,
    const glm::vec3& baselineDirection,
    const glm::vec3& upDirection,
    std::string_view text,
    HorizontalJustify justify,
    const Style& style)
{
    addTextGroupLocal(
        localPositionFromWorldPosition(center),
        baselineDirection,
        upDirection,
        explicitSizeFromUpVector(upDirection),
        text,
        justify,
        true,
        style);
}

void WorldTextRenderer::upload(SDL_GPUCopyPass* copyPass)
{
    HELLO_PROFILE_SCOPE("WorldTextRenderer::Upload");

    if (m_groups.empty() || m_glyphInstances.empty())
    {
        return;
    }

    uploadBufferPair(copyPass, &m_groupBuffer, m_groups.data(), static_cast<std::uint32_t>(m_groups.size()));
    uploadBufferPair(copyPass, &m_glyphInstanceBuffer, m_glyphInstances.data(), static_cast<std::uint32_t>(m_glyphInstances.size()));
    uploadBufferPair(copyPass, &m_styleBuffer, m_styles.data(), static_cast<std::uint32_t>(m_styles.size()));
    uploadBufferPair(copyPass, &m_indirectBuffer, m_drawCommands.data(), static_cast<std::uint32_t>(m_drawCommands.size()));
    uploadBufferPair(copyPass, &m_glyphMetricBuffer, m_glyphMetrics.data(), static_cast<std::uint32_t>(m_glyphMetrics.size()));
}

void WorldTextRenderer::render(SDL_GPURenderPass* renderPass, SDL_GPUCommandBuffer* commandBuffer, const glm::mat4& viewProjection) const
{
    HELLO_PROFILE_SCOPE("WorldTextRenderer::Render");

    if (m_drawCommands.empty() || m_fontTexture == nullptr || m_fontSampler == nullptr)
    {
        return;
    }

    SDL_BindGPUGraphicsPipeline(renderPass, m_pipeline);

    const SDL_GPUBufferBinding vertexBinding{ m_quadVertexBuffer, 0 };
    SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);
    const SDL_GPUBufferBinding indexBinding{ m_quadIndexBuffer, 0 };
    SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);

    SDL_GPUBuffer* vertexStorageBuffers[]{ m_groupBuffer.buffer };
    SDL_BindGPUVertexStorageBuffers(renderPass, 0, vertexStorageBuffers, 1);

    SDL_GPUBuffer* fragmentStorageBuffers[]{
        m_groupBuffer.buffer,
        m_glyphInstanceBuffer.buffer,
        m_glyphMetricBuffer.buffer,
        m_styleBuffer.buffer,
    };
    SDL_BindGPUFragmentStorageBuffers(renderPass, 0, fragmentStorageBuffers, 4);

    const SDL_GPUTextureSamplerBinding samplerBinding{ m_fontTexture, m_fontSampler };
    SDL_BindGPUFragmentSamplers(renderPass, 0, &samplerBinding, 1);

    const VertexUniforms vertexUniforms{ .viewProjection = viewProjection };
    SDL_PushGPUVertexUniformData(commandBuffer, 0, &vertexUniforms, sizeof(vertexUniforms));
    const FragmentUniforms fragmentUniforms{ .distanceRange = m_distanceRange };
    SDL_PushGPUFragmentUniformData(commandBuffer, 0, &fragmentUniforms, sizeof(fragmentUniforms));

    SDL_DrawGPUIndexedPrimitivesIndirect(
        renderPass,
        m_indirectBuffer.buffer,
        0,
        static_cast<Uint32>(m_drawCommands.size()));
}

void WorldTextRenderer::createPipeline(const std::filesystem::path& shaderDirectory)
{
    SDL_GPUShader* vertexShader = createShader(shaderDirectory / "world_text.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 1, 1);
    SDL_GPUShader* fragmentShader = createShader(shaderDirectory / "world_text.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 4, 1);

    SDL_GPUVertexBufferDescription vertexBufferDescription{};
    vertexBufferDescription.slot = 0;
    vertexBufferDescription.pitch = sizeof(QuadVertex);
    vertexBufferDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute vertexAttributes[1]{};
    vertexAttributes[0].location = 0;
    vertexAttributes[0].buffer_slot = 0;
    vertexAttributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    vertexAttributes[0].offset = offsetof(QuadVertex, corner);

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
    pipelineInfo.depth_stencil_state.enable_depth_write = false;
    pipelineInfo.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    pipelineInfo.target_info.num_color_targets = 1;
    pipelineInfo.target_info.color_target_descriptions = &colorTargetDescription;
    pipelineInfo.target_info.has_depth_stencil_target = true;
    pipelineInfo.target_info.depth_stencil_format = m_depthFormat;
    pipelineInfo.vertex_input_state.num_vertex_buffers = 1;
    pipelineInfo.vertex_input_state.vertex_buffer_descriptions = &vertexBufferDescription;
    pipelineInfo.vertex_input_state.num_vertex_attributes = 1;
    pipelineInfo.vertex_input_state.vertex_attributes = vertexAttributes;

    m_pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
    SDL_ReleaseGPUShader(m_device, fragmentShader);
    SDL_ReleaseGPUShader(m_device, vertexShader);

    if (m_pipeline == nullptr)
    {
        throwSdlError("Failed to create world text graphics pipeline.");
    }
}

void WorldTextRenderer::loadFontAssets()
{
    RuntimeAssets::LoadedAssetBinView assetBin;
    RuntimeAssets::LoadedTexBinView texBin;
    std::string error;
    const std::filesystem::path assetRoot = executableRelativePath("assets/runtime");
    if (!RuntimeAssets::LoadAssetBinFromSDL((assetRoot / "roboto.assetbin").string().c_str(), &assetBin, &error) ||
        !RuntimeAssets::LoadTexBinFromSDL((assetRoot / "roboto.texbin").string().c_str(), assetBin, &texBin, &error))
    {
        throw std::runtime_error("Failed to load Roboto runtime assets: " + error);
    }

    if (assetBin.fontAtlases.empty())
    {
        throw std::runtime_error("Roboto runtime asset pack does not contain a font atlas.");
    }

    const RuntimeAssets::FontAtlasRecord& atlas = assetBin.fontAtlases.front();
    if (atlas.textureIndex >= texBin.textures.size())
    {
        throw std::runtime_error("Roboto font atlas references a missing texture.");
    }

    const RuntimeAssets::TextureRecord& texture = texBin.textures[atlas.textureIndex];
    const RuntimeAssets::TextureFormat runtimeFormat = static_cast<RuntimeAssets::TextureFormat>(texture.format);
    if (texture.dimension != static_cast<std::uint32_t>(RuntimeAssets::TextureDimension::Texture2D) ||
        texture.layerCount != 1u ||
        runtimeFormat != RuntimeAssets::TextureFormat::RGBA8_UNORM)
    {
        throw std::runtime_error("Roboto font atlas texture metadata is not the expected RGBA8 2D format.");
    }

    m_fontPixelSize = std::max(atlas.pixelSize, 1.0f);
    m_distanceRange = std::max(atlas.distanceRange, 1.0f);
    m_asciiGlyphs = {};
    m_glyphMetrics.assign(m_asciiGlyphs.size(), GlyphMetricGpu{});
    m_lineTop = 0.0f;
    m_lineBottom = 0.0f;

    const std::uint32_t glyphEnd = atlas.firstGlyph + atlas.glyphCount;
    if (glyphEnd > assetBin.fontGlyphs.size())
    {
        throw std::runtime_error("Roboto font atlas glyph range points past the glyph table.");
    }

    for (std::uint32_t glyphIndex = atlas.firstGlyph; glyphIndex < glyphEnd; ++glyphIndex)
    {
        const RuntimeAssets::FontGlyphRecord& record = assetBin.fontGlyphs[glyphIndex];
        if (record.codepoint >= m_asciiGlyphs.size())
        {
            continue;
        }

        Glyph glyph{};
        glyph.valid = true;
        glyph.codepoint = record.codepoint;
        glyph.u0 = static_cast<float>(record.atlasX) / static_cast<float>(texture.width);
        glyph.v0 = static_cast<float>(record.atlasY) / static_cast<float>(texture.height);
        glyph.u1 = static_cast<float>(record.atlasX + record.atlasWidth) / static_cast<float>(texture.width);
        glyph.v1 = static_cast<float>(record.atlasY + record.atlasHeight) / static_cast<float>(texture.height);
        glyph.planeLeft = record.planeLeft;
        glyph.planeBottom = record.planeBottom;
        glyph.planeRight = record.planeRight;
        glyph.planeTop = record.planeTop;
        glyph.advance = record.advance;
        m_asciiGlyphs[record.codepoint] = glyph;

        m_glyphMetrics[record.codepoint] = GlyphMetricGpu{
            .uvRect = glm::vec4(glyph.u0, glyph.v0, glyph.u1, glyph.v1),
            .planeBounds = glm::vec4(glyph.planeLeft, glyph.planeBottom, glyph.planeRight, glyph.planeTop),
        };

        if (record.codepoint != ' ')
        {
            m_lineTop = std::max(m_lineTop, record.planeTop);
            m_lineBottom = std::min(m_lineBottom, record.planeBottom);
        }
    }

    const SDL_GPUTextureFormat gpuFormat = textureFormatFromRuntimeFormat(runtimeFormat);
    if (!SDL_GPUTextureSupportsFormat(m_device, gpuFormat, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER))
    {
        throw std::runtime_error("SDL GPU device does not support the Roboto atlas texture format.");
    }

    SDL_GPUTextureCreateInfo textureInfo{};
    textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
    textureInfo.format = gpuFormat;
    textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    textureInfo.width = texture.width;
    textureInfo.height = texture.height;
    textureInfo.layer_count_or_depth = 1;
    textureInfo.num_levels = texture.mipCount;
    textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    m_fontTexture = SDL_CreateGPUTexture(m_device, &textureInfo);
    if (m_fontTexture == nullptr)
    {
        throwSdlError("Failed to create Roboto font atlas texture.");
    }

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<Uint32>(texture.dataUncompressedSize);
    SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
    if (transferBuffer == nullptr)
    {
        throwSdlError("Failed to create Roboto font atlas transfer buffer.");
    }

    void* mapped = SDL_MapGPUTransferBuffer(m_device, transferBuffer, false);
    std::memcpy(mapped, texBin.pixelData.data() + texture.dataOffset, static_cast<std::size_t>(texture.dataUncompressedSize));
    SDL_UnmapGPUTransferBuffer(m_device, transferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    std::uint64_t sourceOffset = 0u;
    for (std::uint32_t mipIndex = 0; mipIndex < texture.mipCount; ++mipIndex)
    {
        const std::uint32_t mipWidth = RuntimeAssets::TextureMipExtent(texture.width, mipIndex);
        const std::uint32_t mipHeight = RuntimeAssets::TextureMipExtent(texture.height, mipIndex);
        const std::uint64_t mipByteSize = RuntimeAssets::CalculateTextureMipByteSize(runtimeFormat, mipWidth, mipHeight);

        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = transferBuffer;
        source.offset = static_cast<Uint32>(sourceOffset);
        source.pixels_per_row = mipWidth;
        source.rows_per_layer = mipHeight;

        SDL_GPUTextureRegion destination{};
        destination.texture = m_fontTexture;
        destination.mip_level = mipIndex;
        destination.w = mipWidth;
        destination.h = mipHeight;
        destination.d = 1;
        SDL_UploadToGPUTexture(copyPass, &source, &destination, false);
        sourceOffset += mipByteSize;
    }

    SDL_EndGPUCopyPass(copyPass);
    const bool submitted = SDL_SubmitGPUCommandBuffer(commandBuffer);
    SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
    if (!submitted)
    {
        throwSdlError("Failed to upload Roboto font atlas texture.");
    }
}

void WorldTextRenderer::createSampler()
{
    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.enable_anisotropy = false;
    samplerInfo.max_lod = 1000.0f;

    m_fontSampler = SDL_CreateGPUSampler(m_device, &samplerInfo);
    if (m_fontSampler == nullptr)
    {
        throwSdlError("Failed to create Roboto font sampler.");
    }
}

void WorldTextRenderer::createQuadBuffers()
{
    constexpr QuadVertex kQuadVertices[]{
        { glm::vec2(0.0f, 0.0f) },
        { glm::vec2(1.0f, 0.0f) },
        { glm::vec2(1.0f, 1.0f) },
        { glm::vec2(0.0f, 1.0f) },
    };
    constexpr std::uint16_t kQuadIndices[]{ 0, 1, 2, 0, 2, 3 };

    SDL_GPUBufferCreateInfo vertexInfo{};
    vertexInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertexInfo.size = sizeof(kQuadVertices);
    m_quadVertexBuffer = SDL_CreateGPUBuffer(m_device, &vertexInfo);
    if (m_quadVertexBuffer == nullptr)
    {
        throwSdlError("Failed to create world text quad vertex buffer.");
    }

    SDL_GPUTransferBufferCreateInfo vertexTransferInfo{};
    vertexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    vertexTransferInfo.size = vertexInfo.size;
    m_quadVertexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &vertexTransferInfo);
    if (m_quadVertexTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create world text quad vertex transfer buffer.");
    }

    SDL_GPUBufferCreateInfo indexInfo{};
    indexInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    indexInfo.size = sizeof(kQuadIndices);
    m_quadIndexBuffer = SDL_CreateGPUBuffer(m_device, &indexInfo);
    if (m_quadIndexBuffer == nullptr)
    {
        throwSdlError("Failed to create world text quad index buffer.");
    }

    SDL_GPUTransferBufferCreateInfo indexTransferInfo{};
    indexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    indexTransferInfo.size = indexInfo.size;
    m_quadIndexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &indexTransferInfo);
    if (m_quadIndexTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create world text quad index transfer buffer.");
    }

    void* mappedVertices = SDL_MapGPUTransferBuffer(m_device, m_quadVertexTransferBuffer, false);
    std::memcpy(mappedVertices, kQuadVertices, sizeof(kQuadVertices));
    SDL_UnmapGPUTransferBuffer(m_device, m_quadVertexTransferBuffer);

    void* mappedIndices = SDL_MapGPUTransferBuffer(m_device, m_quadIndexTransferBuffer, false);
    std::memcpy(mappedIndices, kQuadIndices, sizeof(kQuadIndices));
    SDL_UnmapGPUTransferBuffer(m_device, m_quadIndexTransferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);

    SDL_GPUTransferBufferLocation vertexSource{};
    vertexSource.transfer_buffer = m_quadVertexTransferBuffer;
    SDL_GPUBufferRegion vertexDestination{};
    vertexDestination.buffer = m_quadVertexBuffer;
    vertexDestination.size = sizeof(kQuadVertices);
    SDL_UploadToGPUBuffer(copyPass, &vertexSource, &vertexDestination, false);

    SDL_GPUTransferBufferLocation indexSource{};
    indexSource.transfer_buffer = m_quadIndexTransferBuffer;
    SDL_GPUBufferRegion indexDestination{};
    indexDestination.buffer = m_quadIndexBuffer;
    indexDestination.size = sizeof(kQuadIndices);
    SDL_UploadToGPUBuffer(copyPass, &indexSource, &indexDestination, false);

    SDL_EndGPUCopyPass(copyPass);
    if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
    {
        throwSdlError("Failed to upload world text quad buffers.");
    }
}

void WorldTextRenderer::destroyFontResources()
{
    if (m_fontSampler != nullptr)
    {
        SDL_ReleaseGPUSampler(m_device, m_fontSampler);
        m_fontSampler = nullptr;
    }
    if (m_fontTexture != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_fontTexture);
        m_fontTexture = nullptr;
    }
}

void WorldTextRenderer::destroyBufferPair(BufferPair* pair)
{
    if (pair->transfer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, pair->transfer);
        pair->transfer = nullptr;
    }
    if (pair->buffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, pair->buffer);
        pair->buffer = nullptr;
    }
    pair->capacity = 0;
}

void WorldTextRenderer::ensureBufferPairCapacity(BufferPair* pair, std::uint32_t requiredElementCount)
{
    if (requiredElementCount <= pair->capacity)
    {
        return;
    }

    const std::uint32_t newCapacity = std::max<std::uint32_t>(requiredElementCount, std::max<std::uint32_t>(pair->capacity * 2u, 16u));
    destroyBufferPair(pair);

    SDL_GPUBufferCreateInfo bufferInfo{};
    bufferInfo.usage = pair->usage;
    bufferInfo.size = static_cast<Uint32>(pair->elementSize * newCapacity);
    pair->buffer = SDL_CreateGPUBuffer(m_device, &bufferInfo);
    if (pair->buffer == nullptr)
    {
        throwSdlError((std::string("Failed to create ") + pair->name + " buffer.").c_str());
    }

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = bufferInfo.size;
    pair->transfer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
    if (pair->transfer == nullptr)
    {
        throwSdlError((std::string("Failed to create ") + pair->name + " transfer buffer.").c_str());
    }

    pair->capacity = newCapacity;
}

void WorldTextRenderer::uploadBufferPair(SDL_GPUCopyPass* copyPass, BufferPair* pair, const void* data, std::uint32_t elementCount)
{
    if (elementCount == 0u)
    {
        return;
    }

    ensureBufferPairCapacity(pair, elementCount);
    const Uint32 byteSize = static_cast<Uint32>(pair->elementSize * elementCount);
    void* mapped = SDL_MapGPUTransferBuffer(m_device, pair->transfer, true);
    std::memcpy(mapped, data, byteSize);
    SDL_UnmapGPUTransferBuffer(m_device, pair->transfer);

    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = pair->transfer;
    source.offset = 0;
    SDL_GPUBufferRegion destination{};
    destination.buffer = pair->buffer;
    destination.offset = 0;
    destination.size = byteSize;
    SDL_UploadToGPUBuffer(copyPass, &source, &destination, true);
}

void WorldTextRenderer::addTextGroupLocal(
    const glm::vec3& anchor,
    const glm::vec3& baselineDirection,
    const glm::vec3& upDirection,
    float sizeMeters,
    std::string_view text,
    HorizontalJustify justify,
    bool centerBlock,
    const Style& style)
{
    const std::vector<std::string_view> lines = splitLines(text);
    if (lines.empty() || sizeMeters <= 0.0001f)
    {
        return;
    }

    const auto [right, up] = normalizeTextBasis(baselineDirection, upDirection);
    const float scale = sizeMeters / std::max(m_fontPixelSize, 1.0f);
    const float lineAdvance = sizeMeters * style.lineSpacing;
    const float blockTop = m_lineTop * scale;
    const float blockBottom = (-lineAdvance * static_cast<float>(lines.size() - 1u)) + (m_lineBottom * scale);
    const float blockCenterOffset = centerBlock ? ((blockTop + blockBottom) * 0.5f) : 0.0f;

    const float glowOffsetPadding = std::max(std::abs(style.glowOffset.x), std::abs(style.glowOffset.y));
    const float effectPadding = std::max(style.strokeWidth, style.glowWidth + glowOffsetPadding);

    std::vector<PendingGlyph> groupGlyphs;
    for (std::size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex)
    {
        const float lineYOffset = (-lineAdvance * static_cast<float>(lineIndex)) - blockCenterOffset;
        appendLineGlyphs(
            lines[lineIndex],
            lineYOffset / std::max(scale, 0.0001f),
            justify,
            effectPadding,
            &groupGlyphs);
    }

    if (groupGlyphs.empty())
    {
        return;
    }

    const std::uint32_t glyphStart = static_cast<std::uint32_t>(m_glyphInstances.size());
    const std::uint32_t glyphCount = static_cast<std::uint32_t>(groupGlyphs.size());
    const std::uint32_t styleIndex = static_cast<std::uint32_t>(m_styles.size());
    glm::vec4 groupDrawBounds{
        groupGlyphs.front().drawBounds.x,
        groupGlyphs.front().drawBounds.y,
        groupGlyphs.front().drawBounds.z,
        groupGlyphs.front().drawBounds.w,
    };
    for (const PendingGlyph& glyph : groupGlyphs)
    {
        groupDrawBounds.x = std::min(groupDrawBounds.x, glyph.drawBounds.x);
        groupDrawBounds.y = std::min(groupDrawBounds.y, glyph.drawBounds.y);
        groupDrawBounds.z = std::max(groupDrawBounds.z, glyph.drawBounds.z);
        groupDrawBounds.w = std::max(groupDrawBounds.w, glyph.drawBounds.w);
    }

    m_styles.push_back(StyleGpu{
        .baseColor = style.baseColor,
        .strokeColor = style.strokeColor,
        .glowColor = style.glowColor,
        .widths = glm::vec4(style.strokeWidth, style.glowWidth, 0.0f, 0.0f),
        .glowOffset = glm::vec4(style.glowOffset, 0.0f, 0.0f),
    });
    m_groups.push_back(GroupGpu{
        .origin = glm::vec4(anchor, 0.0f),
        .right = glm::vec4(right * scale, effectPadding),
        .up = glm::vec4(up * scale, 0.0f),
        .drawBounds = groupDrawBounds,
        .glyphRangeAndStyle = glm::uvec4(glyphStart, glyphCount, styleIndex, 0u),
    });

    m_glyphInstances.reserve(m_glyphInstances.size() + groupGlyphs.size());
    for (const PendingGlyph& glyph : groupGlyphs)
    {
        m_glyphInstances.push_back(glyph.instance);
    }
    m_drawCommands.push_back(SDL_GPUIndexedIndirectDrawCommand{
        .num_indices = 6u,
        .num_instances = 1u,
        .first_index = 0u,
        .vertex_offset = 0,
        .first_instance = 0u,
    });
}

void WorldTextRenderer::appendLineGlyphs(
    std::string_view text,
    float lineYOffset,
    HorizontalJustify justify,
    float effectPadding,
    std::vector<PendingGlyph>* glyphs) const
{
    const float lineWidth = measureLine(text);
    float xOffset = 0.0f;
    if (justify == HorizontalJustify::Center)
    {
        xOffset = -lineWidth * 0.5f;
    }
    else if (justify == HorizontalJustify::Right)
    {
        xOffset = -lineWidth;
    }

    float penX = xOffset;
    for (const char character : text)
    {
        if (character == '\r')
        {
            continue;
        }
        if (character == '\n')
        {
            break;
        }
        if (character == '\t')
        {
            if (const Glyph* space = glyphFor(' '))
            {
                penX += space->advance * 4.0f;
            }
            continue;
        }

        const Glyph* glyph = glyphFor(character);
        if (glyph == nullptr)
        {
            glyph = glyphFor('?');
        }
        if (glyph == nullptr)
        {
            continue;
        }

        if (character != ' ')
        {
            const float drawMinX = penX + glyph->planeLeft - effectPadding;
            const float drawMaxX = penX + glyph->planeRight + effectPadding;
            glyphs->push_back(PendingGlyph{
                .instance = GlyphInstanceGpu{
                    .offsetAndGlyph = glm::vec4(penX, lineYOffset, static_cast<float>(glyphIndexFor(*glyph)), 0.0f),
                },
                .drawBounds = glm::vec4(
                    drawMinX,
                    lineYOffset + m_lineBottom - effectPadding,
                    drawMaxX,
                    lineYOffset + m_lineTop + effectPadding),
            });
        }
        penX += glyph->advance;
    }
}

std::pair<glm::vec3, glm::vec3> WorldTextRenderer::normalizeTextBasis(
    const glm::vec3& baselineDirection,
    const glm::vec3& upDirection) const
{
    glm::vec3 right = baselineDirection;
    if (glm::length(right) <= 0.0001f)
    {
        right = m_billboardRight;
    }
    right = glm::normalize(right);

    glm::vec3 up = upDirection - (right * glm::dot(upDirection, right));
    if (glm::length(up) <= 0.0001f)
    {
        up = m_billboardUp - (right * glm::dot(m_billboardUp, right));
    }
    if (glm::length(up) <= 0.0001f)
    {
        up = std::abs(right.y) < 0.95f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
        up = up - (right * glm::dot(up, right));
    }

    return { right, glm::normalize(up) };
}

float WorldTextRenderer::explicitSizeFromUpVector(const glm::vec3& upDirection) const
{
    const float sizeMeters = glm::length(upDirection);
    return sizeMeters > 0.0001f ? sizeMeters : m_defaultBillboardSizeMeters;
}

float WorldTextRenderer::measureLine(std::string_view text) const
{
    float penX = 0.0f;
    float maxX = 0.0f;
    for (const char character : text)
    {
        if (character == '\r')
        {
            continue;
        }
        if (character == '\n')
        {
            break;
        }
        if (character == '\t')
        {
            if (const Glyph* space = glyphFor(' '))
            {
                penX += space->advance * 4.0f;
                maxX = std::max(maxX, penX);
            }
            continue;
        }

        const Glyph* glyph = glyphFor(character);
        if (glyph == nullptr)
        {
            glyph = glyphFor('?');
        }
        if (glyph == nullptr)
        {
            continue;
        }

        if (character != ' ')
        {
            maxX = std::max(maxX, penX + glyph->planeRight);
        }
        penX += glyph->advance;
        maxX = std::max(maxX, penX);
    }
    return maxX;
}

std::vector<std::string_view> WorldTextRenderer::splitLines(std::string_view text) const
{
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        lines.emplace_back(text.data() + start, end - start);
        if (newline == std::string_view::npos)
        {
            break;
        }
        start = newline + 1u;
    }
    return lines;
}

const WorldTextRenderer::Glyph* WorldTextRenderer::glyphFor(char character) const
{
    const unsigned char codepoint = static_cast<unsigned char>(character);
    if (codepoint >= m_asciiGlyphs.size() || !m_asciiGlyphs[codepoint].valid)
    {
        return nullptr;
    }
    return &m_asciiGlyphs[codepoint];
}

std::uint32_t WorldTextRenderer::glyphIndexFor(const Glyph& glyph) const
{
    return glyph.codepoint < m_glyphMetrics.size() ? glyph.codepoint : static_cast<std::uint32_t>('?');
}
