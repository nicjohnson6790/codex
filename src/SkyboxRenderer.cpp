#include "SkyboxRenderer.hpp"

#include <SDL3_image/SDL_image.h>
#include <SDL3/SDL_stdinc.h>
#include <glm/gtc/matrix_inverse.hpp>

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
constexpr std::array<SkyboxRenderer::Vertex, 6> kFullscreenQuadVertices{{
    {{ -1.0f, -1.0f }},
    {{  1.0f, -1.0f }},
    {{  1.0f,  1.0f }},
    {{ -1.0f, -1.0f }},
    {{  1.0f,  1.0f }},
    {{ -1.0f,  1.0f }},
}};

struct DecodedImage
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

DecodedImage decodePngRgba8(const std::filesystem::path& path)
{
    SDL_Surface* loadedSurface = IMG_Load(path.string().c_str());
    if (loadedSurface == nullptr)
    {
        throw std::runtime_error("Failed to load skybox texture: " + path.string() + " " + SDL_GetError());
    }

    SDL_Surface* rgbaSurface = SDL_ConvertSurface(loadedSurface, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loadedSurface);
    if (rgbaSurface == nullptr)
    {
        throw std::runtime_error("Failed to convert skybox texture to RGBA32: " + path.string() + " " + SDL_GetError());
    }

    DecodedImage image{};
    image.width = static_cast<std::uint32_t>(rgbaSurface->w);
    image.height = static_cast<std::uint32_t>(rgbaSurface->h);
    image.pixels.resize(static_cast<std::size_t>(rgbaSurface->w) * static_cast<std::size_t>(rgbaSurface->h) * 4u);
    std::memcpy(image.pixels.data(), rgbaSurface->pixels, image.pixels.size());
    SDL_DestroySurface(rgbaSurface);

    return image;
}
}

void SkyboxRenderer::initialize(
    SDL_GPUDevice* device,
    SDL_GPUTextureFormat colorFormat,
    SDL_GPUTextureFormat depthFormat,
    const std::filesystem::path& shaderDirectory,
    const std::filesystem::path& resourceDirectory)
{
    initializeRendererBase(device, colorFormat, depthFormat);
    createStaticVertexResources();
    createCubemapTexture(resourceDirectory);
    createPipeline(shaderDirectory);
}

void SkyboxRenderer::shutdown()
{
    if (m_sampler != nullptr)
    {
        SDL_ReleaseGPUSampler(m_device, m_sampler);
        m_sampler = nullptr;
    }
    if (m_cubemapTexture != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_cubemapTexture);
        m_cubemapTexture = nullptr;
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
    if (m_pipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
        m_pipeline = nullptr;
    }
}

void SkyboxRenderer::render(
    SDL_GPURenderPass* renderPass,
    SDL_GPUCommandBuffer* commandBuffer,
    const glm::mat4& inverseViewProjection,
    const LightingSystem& lightingSystem) const
{
    if (m_pipeline == nullptr || m_vertexBuffer == nullptr || m_cubemapTexture == nullptr || m_sampler == nullptr)
    {
        return;
    }

    SDL_BindGPUGraphicsPipeline(renderPass, m_pipeline);

    const SDL_GPUBufferBinding vertexBinding{ m_vertexBuffer, 0 };
    SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

    const SDL_GPUTextureSamplerBinding samplerBinding{
        m_cubemapTexture,
        m_sampler,
    };
    SDL_BindGPUFragmentSamplers(renderPass, 0, &samplerBinding, 1);

    FragmentUniforms uniforms{};
    uniforms.inverseViewProjection = inverseViewProjection;
    uniforms.skyRotation = lightingSystem.skyboxRotationMatrix();
    SDL_PushGPUFragmentUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

    SDL_DrawGPUPrimitives(renderPass, static_cast<Uint32>(kFullscreenQuadVertices.size()), 1, 0, 0);
}

void SkyboxRenderer::createPipeline(const std::filesystem::path& shaderDirectory)
{
    SDL_GPUShader* vertexShader = createShader(shaderDirectory / "skybox.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* fragmentShader = createShader(shaderDirectory / "skybox.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, 1);

    SDL_GPUVertexBufferDescription vertexBufferDescription{};
    vertexBufferDescription.slot = 0;
    vertexBufferDescription.pitch = sizeof(Vertex);
    vertexBufferDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute vertexAttributes[1]{};
    vertexAttributes[0].location = 0;
    vertexAttributes[0].buffer_slot = 0;
    vertexAttributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    vertexAttributes[0].offset = offsetof(Vertex, position);

    SDL_GPUColorTargetDescription colorTargetDescription{};
    colorTargetDescription.format = m_colorFormat;

    SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.vertex_shader = vertexShader;
    pipelineInfo.fragment_shader = fragmentShader;
    pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipelineInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipelineInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipelineInfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipelineInfo.rasterizer_state.enable_depth_clip = false;
    pipelineInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pipelineInfo.depth_stencil_state.enable_depth_test = true;
    pipelineInfo.depth_stencil_state.enable_depth_write = false;
    pipelineInfo.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_EQUAL;
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
        throwSdlError("Failed to create skybox graphics pipeline.");
    }

    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.min_lod = 0.0f;
    samplerInfo.max_lod = 0.0f;
    m_sampler = SDL_CreateGPUSampler(m_device, &samplerInfo);
    if (m_sampler == nullptr)
    {
        throwSdlError("Failed to create skybox sampler.");
    }
}

void SkyboxRenderer::createStaticVertexResources()
{
    SDL_GPUBufferCreateInfo vertexInfo{};
    vertexInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertexInfo.size = static_cast<Uint32>(sizeof(Vertex) * kFullscreenQuadVertices.size());
    m_vertexBuffer = SDL_CreateGPUBuffer(m_device, &vertexInfo);
    if (m_vertexBuffer == nullptr)
    {
        throwSdlError("Failed to create skybox vertex buffer.");
    }

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = vertexInfo.size;
    m_vertexTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
    if (m_vertexTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create skybox vertex transfer buffer.");
    }

    void* mapped = SDL_MapGPUTransferBuffer(m_device, m_vertexTransferBuffer, false);
    std::memcpy(mapped, kFullscreenQuadVertices.data(), sizeof(Vertex) * kFullscreenQuadVertices.size());
    SDL_UnmapGPUTransferBuffer(m_device, m_vertexTransferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);

    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = m_vertexTransferBuffer;

    SDL_GPUBufferRegion destination{};
    destination.buffer = m_vertexBuffer;
    destination.size = vertexInfo.size;
    SDL_UploadToGPUBuffer(copyPass, &source, &destination, false);
    SDL_EndGPUCopyPass(copyPass);

    if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
    {
        throwSdlError("Failed to upload skybox vertex data.");
    }
}

void SkyboxRenderer::createCubemapTexture(const std::filesystem::path& resourceDirectory)
{
    const std::array<std::filesystem::path, 6> facePaths{
        resourceDirectory / "skybox" / "px.png",
        resourceDirectory / "skybox" / "nx.png",
        resourceDirectory / "skybox" / "py.png",
        resourceDirectory / "skybox" / "ny.png",
        resourceDirectory / "skybox" / "pz.png",
        resourceDirectory / "skybox" / "nz.png",
    };

    std::array<DecodedImage, 6> decodedFaces{};
    for (std::size_t faceIndex = 0; faceIndex < facePaths.size(); ++faceIndex)
    {
        decodedFaces[faceIndex] = decodePngRgba8(facePaths[faceIndex]);
    }

    const std::uint32_t faceWidth = decodedFaces[0].width;
    const std::uint32_t faceHeight = decodedFaces[0].height;
    if (faceWidth == 0 || faceHeight == 0 || faceWidth != faceHeight)
    {
        throw std::runtime_error("Skybox cubemap faces must be non-empty square textures.");
    }

    for (const DecodedImage& face : decodedFaces)
    {
        if (face.width != faceWidth || face.height != faceHeight)
        {
            throw std::runtime_error("All skybox cubemap faces must share the same dimensions.");
        }
    }

    SDL_GPUTextureCreateInfo textureInfo{};
    textureInfo.type = SDL_GPU_TEXTURETYPE_CUBE;
    textureInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    textureInfo.width = faceWidth;
    textureInfo.height = faceHeight;
    textureInfo.layer_count_or_depth = 6;
    textureInfo.num_levels = 1;
    textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    m_cubemapTexture = SDL_CreateGPUTexture(m_device, &textureInfo);
    if (m_cubemapTexture == nullptr)
    {
        throwSdlError("Failed to create skybox cubemap texture.");
    }

    const std::size_t faceSizeBytes = static_cast<std::size_t>(faceWidth) * static_cast<std::size_t>(faceHeight) * 4u;
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<Uint32>(faceSizeBytes * decodedFaces.size());
    SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
    if (transferBuffer == nullptr)
    {
        throwSdlError("Failed to create skybox upload transfer buffer.");
    }

    std::uint8_t* mapped = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(m_device, transferBuffer, false));
    for (std::size_t faceIndex = 0; faceIndex < decodedFaces.size(); ++faceIndex)
    {
        std::memcpy(mapped + (faceIndex * faceSizeBytes), decodedFaces[faceIndex].pixels.data(), faceSizeBytes);
    }
    SDL_UnmapGPUTransferBuffer(m_device, transferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);

    for (std::size_t faceIndex = 0; faceIndex < decodedFaces.size(); ++faceIndex)
    {
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = transferBuffer;
        source.offset = static_cast<Uint32>(faceIndex * faceSizeBytes);
        source.pixels_per_row = faceWidth;
        source.rows_per_layer = faceHeight;

        SDL_GPUTextureRegion destination{};
        destination.texture = m_cubemapTexture;
        destination.layer = static_cast<Uint32>(faceIndex);
        destination.w = faceWidth;
        destination.h = faceHeight;
        destination.d = 1;
        SDL_UploadToGPUTexture(copyPass, &source, &destination, false);
    }

    SDL_EndGPUCopyPass(copyPass);
    if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
    {
        SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
        throwSdlError("Failed to upload skybox cubemap texture.");
    }

    SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
}
