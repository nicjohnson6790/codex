#include "SkyboxRenderer.hpp"

#include "AppConfig.hpp"
#include "QuadtreeWaterMeshRenderer.hpp"
#include "assets/RuntimeAssetReader.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#include <glm/common.hpp>
#include <glm/ext/scalar_constants.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/trigonometric.hpp>

#include <array>
#include <cmath>
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

std::filesystem::path executableRelativePath(const std::filesystem::path& relativePath)
{
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr)
    {
        throw std::runtime_error(std::string("Failed to resolve executable base path: ") + SDL_GetError());
    }

    return std::filesystem::path(basePath) / relativePath;
}
}

void SkyboxRenderer::initialize(
    SDL_GPUDevice* device,
    SDL_GPUTextureFormat colorFormat,
    SDL_GPUTextureFormat depthFormat,
    const std::filesystem::path& shaderDirectory)
{
    initializeRendererBase(device, colorFormat, depthFormat);
    createStaticVertexResources();
    createCubemapTexture();
    createPipeline(shaderDirectory);
}

void SkyboxRenderer::shutdown()
{
    if (m_depthSampler != nullptr)
    {
        SDL_ReleaseGPUSampler(m_device, m_depthSampler);
        m_depthSampler = nullptr;
    }
    if (m_cubemapSampler != nullptr)
    {
        SDL_ReleaseGPUSampler(m_device, m_cubemapSampler);
        m_cubemapSampler = nullptr;
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
    for (auto& pipeline : m_pipelines)
    {
        if (pipeline) SDL_ReleaseGPUGraphicsPipeline(m_device, pipeline);
        pipeline = nullptr;
    }
}

void SkyboxRenderer::render(
    SDL_GPURenderPass* renderPass,
    SDL_GPUCommandBuffer* commandBuffer,
    const glm::mat4& inverseViewProjection,
    SDL_GPUTexture* depthTexture,
    float cameraAltitude,
    const LightingSystem& lightingSystem,
    const QuadtreeWaterMeshRenderer& waterRenderer,
    SDL_GPUBuffer* terrainHeightmapBuffer,
    float viewportHeight) const
{
    if (!m_pipelines[0] || !depthTexture || !waterRenderer.displacementTexture()
        || !terrainHeightmapBuffer) return;
    const SDL_GPUBufferBinding vertexBinding{m_vertexBuffer, 0};
    SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);
    const SDL_GPUTextureSamplerBinding samplers[]{
        {m_cubemapTexture, m_cubemapSampler},
        {depthTexture, m_depthSampler},
        {waterRenderer.displacementTexture(), waterRenderer.waterSampler()},
    };
    FragmentUniforms uniforms{};
    uniforms.inverseViewProjection = inverseViewProjection;
    const auto shared = buildSharedSkyUniforms(cameraAltitude, lightingSystem);
    uniforms.skyRotation = shared.skyRotation;
    uniforms.atmosphereParams = shared.atmosphereParams;
    uniforms.sunDirectionTimeOfDay = shared.sunDirectionTimeOfDay;
    uniforms.optics = buildAtmosphereOptics(lightingSystem);
    waterRenderer.fillMediumUniforms(uniforms, viewportHeight);
    uniforms.waterAbsorption = glm::vec4(m_waterMediumSettings.absorption, 0.0f);
    uniforms.waterScattering = glm::vec4(m_waterMediumSettings.scattering, m_waterMediumSettings.sourceScale);
    for (std::size_t pass = 0; pass < m_pipelines.size(); ++pass)
    {
        SDL_BindGPUGraphicsPipeline(renderPass, m_pipelines[pass]);
        SDL_BindGPUFragmentSamplers(renderPass, 0, samplers, 3);
        SDL_BindGPUFragmentStorageBuffers(renderPass, 0, &terrainHeightmapBuffer, 1);
        uniforms.waterParams.w = static_cast<float>(pass);
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));
        SDL_DrawGPUPrimitives(renderPass, static_cast<Uint32>(kFullscreenQuadVertices.size()), 1, 0, 0);
    }
}

SkyboxRenderer::SharedSkyUniforms SkyboxRenderer::buildSharedSkyUniforms(
    float cameraAltitude,
    const LightingSystem& lightingSystem) const
{
    SharedSkyUniforms uniforms{};
    uniforms.skyRotation = lightingSystem.skyboxRotationMatrix();
    uniforms.atmosphereParams = glm::vec4(
        m_atmosphereSettings.atmosphereHeight,
        m_atmosphereSettings.atmosphereDistanceRange,
        static_cast<float>(AppConfig::Camera::kNearPlane),
        cameraAltitude
    );

    const glm::vec3 sunDirection = lightingSystem.sunDirection();
    uniforms.sunDirectionTimeOfDay = glm::vec4(
        sunDirection.x,
        sunDirection.y,
        sunDirection.z,
        lightingSystem.sun().timeOfDayHours / 24.0f
    );
    return uniforms;
}

void SkyboxRenderer::resetAtmosphereSettings()
{
    m_atmosphereSettings = {};
}

void SkyboxRenderer::sanitizeAtmosphereSettings()
{
    m_atmosphereSettings.atmosphereHeight = std::max(m_atmosphereSettings.atmosphereHeight, 1000.0f);
    m_atmosphereSettings.atmosphereDistanceRange = std::max(m_atmosphereSettings.atmosphereDistanceRange, 1000.0f);
    m_atmosphereSettings.mieG = std::clamp(m_atmosphereSettings.mieG, 0.0f, 0.99f);
    m_waterMediumSettings.sourceScale = std::max(m_waterMediumSettings.sourceScale, 0.0f);
    m_atmosphereSettings.atmosphereCloudSourceScale = std::max(m_atmosphereSettings.atmosphereCloudSourceScale, 0.01f);
    m_atmosphereSettings.rayleighScaleHeight = std::max(m_atmosphereSettings.rayleighScaleHeight, 1.0f);
    m_atmosphereSettings.mieScaleHeight = std::max(m_atmosphereSettings.mieScaleHeight, 1.0f);
    m_atmosphereSettings.ozoneColumnHeight = std::max(m_atmosphereSettings.ozoneColumnHeight, 0.0f);
    m_atmosphereSettings.mieScatter = std::max(m_atmosphereSettings.mieScatter, 0.0f);
    m_atmosphereSettings.mieExtinction = std::max(m_atmosphereSettings.mieExtinction, m_atmosphereSettings.mieScatter);
    m_waterMediumSettings.absorption = glm::max(m_waterMediumSettings.absorption, glm::vec3(0.0f));
    m_waterMediumSettings.scattering = glm::max(m_waterMediumSettings.scattering, glm::vec3(0.0f));
}


SkyboxRenderer::AtmosphereOptics SkyboxRenderer::buildAtmosphereOptics(const LightingSystem& lighting) const
{
    const auto& a = m_atmosphereSettings;
    AtmosphereOptics result{};
    result.rayleigh = glm::vec4(glm::max(glm::vec3(a.rayleighScatterR, a.rayleighScatterG, a.rayleighScatterB), glm::vec3(0.0f)), a.rayleighScaleHeight);
    result.mie = glm::vec4(a.mieScatter, a.mieExtinction, a.mieScaleHeight, a.mieG);
    result.ozone = glm::vec4(glm::max(glm::vec3(a.ozoneAbsorptionR, a.ozoneAbsorptionG, a.ozoneAbsorptionB), glm::vec3(0.0f)), a.ozoneColumnHeight);
    result.radianceScales = glm::vec4(AppConfig::Atmosphere::kEnvironmentRadianceScale,
        AppConfig::Atmosphere::kSpaceRadiance, AppConfig::Atmosphere::kSunDiskRadiance, 0.0f);
    result.solar = glm::vec4(glm::max(lighting.sun().color * lighting.sun().intensity, glm::vec3(0.0f)), a.atmosphereCloudSourceScale);
    return result;
}

void SkyboxRenderer::createPipeline(const std::filesystem::path& shaderDirectory)
{
    SDL_GPUShader* vertexShader = createShader(shaderDirectory / "skybox.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* fragmentShader = createShader(shaderDirectory / "skybox.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1, 3);

    SDL_GPUVertexBufferDescription vertexBufferDescription{};
    vertexBufferDescription.slot = 0;
    vertexBufferDescription.pitch = sizeof(Vertex);
    vertexBufferDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute vertexAttributes[1]{};
    vertexAttributes[0].location = 0;
    vertexAttributes[0].buffer_slot = 0;
    vertexAttributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    vertexAttributes[0].offset = offsetof(Vertex, position);

    SDL_GPUColorTargetBlendState blendState{};
    blendState.enable_blend = true;
    blendState.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blendState.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    blendState.color_blend_op = SDL_GPU_BLENDOP_ADD;
    blendState.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    blendState.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
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
    pipelineInfo.rasterizer_state.enable_depth_clip = false;
    pipelineInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pipelineInfo.depth_stencil_state.enable_depth_test = false;
    pipelineInfo.depth_stencil_state.enable_depth_write = false;
    pipelineInfo.target_info.num_color_targets = 1;
    pipelineInfo.target_info.color_target_descriptions = &colorTargetDescription;
    pipelineInfo.vertex_input_state.num_vertex_buffers = 1;
    pipelineInfo.vertex_input_state.vertex_buffer_descriptions = &vertexBufferDescription;
    pipelineInfo.vertex_input_state.num_vertex_attributes = 1;
    pipelineInfo.vertex_input_state.vertex_attributes = vertexAttributes;

    for (std::size_t pass = 0; pass < m_pipelines.size(); ++pass)
    {
        // Background replaces only depth-zero pixels (shader discard).
        // RGB multiplication and addition preserve destination alpha.
        colorTargetDescription.blend_state.src_color_blendfactor =
            pass == 1 ? SDL_GPU_BLENDFACTOR_ZERO : SDL_GPU_BLENDFACTOR_ONE;
        colorTargetDescription.blend_state.dst_color_blendfactor =
            pass == 0 ? SDL_GPU_BLENDFACTOR_ZERO :
            pass == 1 ? SDL_GPU_BLENDFACTOR_SRC_COLOR : SDL_GPU_BLENDFACTOR_ONE;
        m_pipelines[pass] = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
    }
    SDL_ReleaseGPUShader(m_device, fragmentShader);
    SDL_ReleaseGPUShader(m_device, vertexShader);
    for (auto* pipeline : m_pipelines)
        if (!pipeline) throwSdlError("Failed to create sky/medium graphics pipeline.");

    SDL_GPUSamplerCreateInfo cubemapSamplerInfo{};
    cubemapSamplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    cubemapSamplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    cubemapSamplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    cubemapSamplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    cubemapSamplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    cubemapSamplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    cubemapSamplerInfo.min_lod = 0.0f;
    cubemapSamplerInfo.max_lod = 0.0f;
    m_cubemapSampler = SDL_CreateGPUSampler(m_device, &cubemapSamplerInfo);
    if (m_cubemapSampler == nullptr)
    {
        throwSdlError("Failed to create skybox cubemap sampler.");
    }

    SDL_GPUSamplerCreateInfo depthSamplerInfo{};
    depthSamplerInfo.min_filter = SDL_GPU_FILTER_NEAREST;
    depthSamplerInfo.mag_filter = SDL_GPU_FILTER_NEAREST;
    depthSamplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    depthSamplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    depthSamplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    depthSamplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    depthSamplerInfo.min_lod = 0.0f;
    depthSamplerInfo.max_lod = 0.0f;
    m_depthSampler = SDL_CreateGPUSampler(m_device, &depthSamplerInfo);
    if (m_depthSampler == nullptr)
    {
        throwSdlError("Failed to create depth sampler.");
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

void SkyboxRenderer::createCubemapTexture()
{
    RuntimeAssets::LoadedAssetBinView assetBin;
    RuntimeAssets::LoadedTexBinView texBin;
    std::string error;
    const std::filesystem::path assetRoot = executableRelativePath("assets/runtime");
    if (!RuntimeAssets::LoadAssetBinFromSDL((assetRoot / "skybox.assetbin").string().c_str(), &assetBin, &error) ||
        !RuntimeAssets::LoadTexBinFromSDL((assetRoot / "skybox.texbin").string().c_str(), assetBin, &texBin, &error))
    {
        throw std::runtime_error("Failed to load skybox runtime assets: " + error);
    }

    const std::array<const char*, 6> faceNames{
        "px",
        "nx",
        "py",
        "ny",
        "pz",
        "nz",
    };

    std::array<const RuntimeAssets::TextureRecord*, 6> faceTextures{};
    for (std::size_t faceIndex = 0; faceIndex < faceNames.size(); ++faceIndex)
    {
        for (const RuntimeAssets::TextureRecord& texture : texBin.textures)
        {
            if (std::strcmp(texBin.stringAt(texture.nameOffset), faceNames[faceIndex]) == 0)
            {
                faceTextures[faceIndex] = &texture;
                break;
            }
        }
        if (faceTextures[faceIndex] == nullptr)
        {
            throw std::runtime_error(std::string("Skybox runtime pack is missing face texture: ") + faceNames[faceIndex]);
        }
    }

    const std::uint32_t faceWidth = faceTextures[0]->width;
    const std::uint32_t faceHeight = faceTextures[0]->height;
    if (faceWidth == 0 || faceHeight == 0 || faceWidth != faceHeight)
    {
        throw std::runtime_error("Skybox cubemap faces must be non-empty square textures.");
    }

    for (const RuntimeAssets::TextureRecord* face : faceTextures)
    {
        if (face->width != faceWidth || face->height != faceHeight)
        {
            throw std::runtime_error("All skybox cubemap faces must share the same dimensions.");
        }
    }

    SDL_GPUTextureCreateInfo textureInfo{};
    textureInfo.type = SDL_GPU_TEXTURETYPE_CUBE;
    textureInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
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
    transferInfo.size = static_cast<Uint32>(faceSizeBytes * faceTextures.size());
    SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
    if (transferBuffer == nullptr)
    {
        throwSdlError("Failed to create skybox upload transfer buffer.");
    }

    std::uint8_t* mapped = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(m_device, transferBuffer, false));
    for (std::size_t faceIndex = 0; faceIndex < faceTextures.size(); ++faceIndex)
    {
        const RuntimeAssets::TextureRecord& face = *faceTextures[faceIndex];
        std::memcpy(
            mapped + (faceIndex * faceSizeBytes),
            texBin.pixelData.data() + face.dataOffset,
            faceSizeBytes);
    }
    SDL_UnmapGPUTransferBuffer(m_device, transferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);

    for (std::size_t faceIndex = 0; faceIndex < faceTextures.size(); ++faceIndex)
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
