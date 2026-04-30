#include "SkyboxRenderer.hpp"

#include "AppConfig.hpp"

#include <SDL3_image/SDL_image.h>
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

float saturate(float value)
{
    return glm::clamp(value, 0.0f, 1.0f);
}

glm::vec3 saturate(const glm::vec3& value)
{
    return glm::clamp(value, glm::vec3(0.0f), glm::vec3(1.0f));
}

float decodeLogDistance(float distanceT, float maxDistance)
{
    constexpr float kDistanceLogBase = 256.0f;
    const float scaled = (std::pow(kDistanceLogBase, glm::clamp(distanceT, 0.0f, 1.0f)) - 1.0f) / (kDistanceLogBase - 1.0f);
    return scaled * maxDistance;
}

float luminance(const glm::vec3& color)
{
    return glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
}

glm::vec3 expVec3(const glm::vec3& value)
{
    return glm::vec3(
        std::exp(value.x),
        std::exp(value.y),
        std::exp(value.z));
}

float rayleighPhase(float viewSunDot)
{
    return (3.0f / (16.0f * glm::pi<float>())) * (1.0f + (viewSunDot * viewSunDot));
}

float henyeyGreensteinPhase(float viewSunDot, float g)
{
    const float g2 = g * g;
    const float denominator = std::pow(std::max(1.0f + g2 - (2.0f * g * viewSunDot), 1.0e-3f), 1.5f);
    return (1.0f / (4.0f * glm::pi<float>())) * ((1.0f - g2) / denominator);
}

float approximateAirMass(float sunHeight)
{
    const float sunHeightClamped = glm::clamp(sunHeight, -0.12f, 1.0f);
    const float elevationDegrees = glm::degrees(std::asin(sunHeightClamped));
    const float safeElevationDegrees = std::max(elevationDegrees, -5.9f);
    const float denominator =
        std::sin(glm::radians(safeElevationDegrees)) +
        (0.50572f * std::pow(safeElevationDegrees + 6.07995f, -1.6364f));
    return 1.0f / std::max(denominator, 0.08f);
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
    createAtmosphereLutTexture();
    createPipeline(shaderDirectory);
}

void SkyboxRenderer::shutdown()
{
    if (m_depthSampler != nullptr)
    {
        SDL_ReleaseGPUSampler(m_device, m_depthSampler);
        m_depthSampler = nullptr;
    }
    if (m_atmosphereSampler != nullptr)
    {
        SDL_ReleaseGPUSampler(m_device, m_atmosphereSampler);
        m_atmosphereSampler = nullptr;
    }
    if (m_cubemapSampler != nullptr)
    {
        SDL_ReleaseGPUSampler(m_device, m_cubemapSampler);
        m_cubemapSampler = nullptr;
    }
    if (m_atmosphereLutTexture != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_atmosphereLutTexture);
        m_atmosphereLutTexture = nullptr;
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
    SDL_GPUTexture* depthTexture,
    float cameraAltitude,
    const LightingSystem& lightingSystem) const
{
    if (m_pipeline == nullptr ||
        m_vertexBuffer == nullptr ||
        m_cubemapTexture == nullptr ||
        m_atmosphereLutTexture == nullptr ||
        depthTexture == nullptr ||
        m_cubemapSampler == nullptr ||
        m_atmosphereSampler == nullptr ||
        m_depthSampler == nullptr)
    {
        return;
    }

    SDL_BindGPUGraphicsPipeline(renderPass, m_pipeline);

    const SDL_GPUBufferBinding vertexBinding{ m_vertexBuffer, 0 };
    SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

    const SDL_GPUTextureSamplerBinding samplerBindings[3]{
        { m_cubemapTexture, m_cubemapSampler },
        { m_atmosphereLutTexture, m_atmosphereSampler },
        { depthTexture, m_depthSampler },
    };
    SDL_BindGPUFragmentSamplers(renderPass, 0, samplerBindings, 3);

    FragmentUniforms uniforms{};
    uniforms.inverseViewProjection = inverseViewProjection;
    const SharedSkyUniforms sharedUniforms = buildSharedSkyUniforms(cameraAltitude, lightingSystem);
    uniforms.skyRotation = sharedUniforms.skyRotation;
    uniforms.atmosphereParams = sharedUniforms.atmosphereParams;
    uniforms.sunDirectionTimeOfDay = sharedUniforms.sunDirectionTimeOfDay;
    SDL_PushGPUFragmentUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));

    SDL_DrawGPUPrimitives(renderPass, static_cast<Uint32>(kFullscreenQuadVertices.size()), 1, 0, 0);
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

void SkyboxRenderer::regenerateAtmosphereLut()
{
    sanitizeAtmosphereSettings();
    createAtmosphereLutTexture();
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
    m_atmosphereSettings.exposure = std::max(m_atmosphereSettings.exposure, 0.01f);
    m_atmosphereSettings.alphaScale = std::max(m_atmosphereSettings.alphaScale, 0.01f);
    m_atmosphereSettings.rayleighScaleHeight = std::max(m_atmosphereSettings.rayleighScaleHeight, 1.0f);
    m_atmosphereSettings.mieScaleHeight = std::max(m_atmosphereSettings.mieScaleHeight, 1.0f);
    m_atmosphereSettings.ozoneColumnHeight = std::max(m_atmosphereSettings.ozoneColumnHeight, 1.0f);
    m_atmosphereSettings.ambientSkyScale = std::max(m_atmosphereSettings.ambientSkyScale, 0.0f);
    m_atmosphereSettings.ambientBlueBias = std::clamp(m_atmosphereSettings.ambientBlueBias, 0.0f, 1.0f);
    m_atmosphereSettings.ambientSolarInfluence = std::clamp(m_atmosphereSettings.ambientSolarInfluence, 0.0f, 1.0f);
    m_atmosphereSettings.ambientTwilightInfluence = std::clamp(m_atmosphereSettings.ambientTwilightInfluence, 0.0f, 1.0f);
    m_atmosphereSettings.rayleighTintScale = std::max(m_atmosphereSettings.rayleighTintScale, 0.0f);
    m_atmosphereSettings.hazeStrength = std::max(m_atmosphereSettings.hazeStrength, 0.0f);
    m_atmosphereSettings.pathFogDistance = std::max(m_atmosphereSettings.pathFogDistance, 1.0f);
    m_atmosphereSettings.longRangeHazeDistance = std::max(m_atmosphereSettings.longRangeHazeDistance, 1.0f);
    m_atmosphereSettings.aureolePower = std::max(m_atmosphereSettings.aureolePower, 1.0f);
    m_atmosphereSettings.aureoleStrength = std::max(m_atmosphereSettings.aureoleStrength, 0.0f);
    m_atmosphereSettings.sunDiskPower = std::max(m_atmosphereSettings.sunDiskPower, 1.0f);
    m_atmosphereSettings.sunDiskStrength = std::max(m_atmosphereSettings.sunDiskStrength, 0.0f);
    m_atmosphereSettings.sunGlowPower = std::max(m_atmosphereSettings.sunGlowPower, 1.0f);
    m_atmosphereSettings.sunsetStrength = std::max(m_atmosphereSettings.sunsetStrength, 0.0f);
    m_atmosphereSettings.sunsetSunwardBoost = std::max(m_atmosphereSettings.sunsetSunwardBoost, 0.0f);
    m_atmosphereSettings.sunsetDistanceMin = std::max(m_atmosphereSettings.sunsetDistanceMin, 0.0f);
    m_atmosphereSettings.sunsetDistanceMax = std::max(m_atmosphereSettings.sunsetDistanceMax, 0.0f);
}

void SkyboxRenderer::createPipeline(const std::filesystem::path& shaderDirectory)
{
    SDL_GPUShader* vertexShader = createShader(shaderDirectory / "skybox.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0);
    SDL_GPUShader* fragmentShader = createShader(shaderDirectory / "skybox.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, 3);

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

    m_pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
    SDL_ReleaseGPUShader(m_device, fragmentShader);
    SDL_ReleaseGPUShader(m_device, vertexShader);
    if (m_pipeline == nullptr)
    {
        throwSdlError("Failed to create skybox graphics pipeline.");
    }

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

    SDL_GPUSamplerCreateInfo atmosphereSamplerInfo{};
    atmosphereSamplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    atmosphereSamplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    atmosphereSamplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    atmosphereSamplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    atmosphereSamplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    atmosphereSamplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    atmosphereSamplerInfo.min_lod = 0.0f;
    atmosphereSamplerInfo.max_lod = 0.0f;
    m_atmosphereSampler = SDL_CreateGPUSampler(m_device, &atmosphereSamplerInfo);
    if (m_atmosphereSampler == nullptr)
    {
        throwSdlError("Failed to create atmosphere LUT sampler.");
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

void SkyboxRenderer::createAtmosphereLutTexture()
{
    sanitizeAtmosphereSettings();
    const auto lutBytes = buildAtmosphereLut();
    SDL_GPUTexture* previousTexture = m_atmosphereLutTexture;
    SDL_GPUTexture* newTexture = nullptr;

    SDL_GPUTextureCreateInfo textureInfo{};
    textureInfo.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    textureInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    textureInfo.width = kAtmosphereLutResolution;
    textureInfo.height = kAtmosphereLutResolution;
    textureInfo.layer_count_or_depth = kAtmosphereLutResolution;
    textureInfo.num_levels = 1;
    textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    newTexture = SDL_CreateGPUTexture(m_device, &textureInfo);
    if (newTexture == nullptr)
    {
        throwSdlError("Failed to create atmosphere LUT texture.");
    }

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<Uint32>(lutBytes.size());
    SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
    if (transferBuffer == nullptr)
    {
        throwSdlError("Failed to create atmosphere LUT upload buffer.");
    }

    void* mapped = SDL_MapGPUTransferBuffer(m_device, transferBuffer, false);
    std::memcpy(mapped, lutBytes.data(), lutBytes.size());
    SDL_UnmapGPUTransferBuffer(m_device, transferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);

    const std::size_t layerSizeBytes =
        static_cast<std::size_t>(kAtmosphereLutResolution) *
        static_cast<std::size_t>(kAtmosphereLutResolution) * 4u;
    for (std::uint32_t layerIndex = 0; layerIndex < kAtmosphereLutResolution; ++layerIndex)
    {
        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = transferBuffer;
        source.offset = static_cast<Uint32>(layerIndex * layerSizeBytes);
        source.pixels_per_row = kAtmosphereLutResolution;
        source.rows_per_layer = kAtmosphereLutResolution;

        SDL_GPUTextureRegion destination{};
        destination.texture = newTexture;
        destination.layer = layerIndex;
        destination.w = kAtmosphereLutResolution;
        destination.h = kAtmosphereLutResolution;
        destination.d = 1;
        SDL_UploadToGPUTexture(copyPass, &source, &destination, false);
    }

    SDL_EndGPUCopyPass(copyPass);
    if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
    {
        SDL_ReleaseGPUTexture(m_device, newTexture);
        SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
        throwSdlError("Failed to upload atmosphere LUT texture.");
    }

    SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
    m_atmosphereLutTexture = newTexture;
    if (previousTexture != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, previousTexture);
    }
}

std::array<std::uint8_t, SkyboxRenderer::kAtmosphereLutResolution * SkyboxRenderer::kAtmosphereLutResolution * SkyboxRenderer::kAtmosphereLutResolution * 4> SkyboxRenderer::buildAtmosphereLut() const
{
    const glm::vec3 rayleighScattering(
        m_atmosphereSettings.rayleighScatterR,
        m_atmosphereSettings.rayleighScatterG,
        m_atmosphereSettings.rayleighScatterB);
    const glm::vec3 mieScattering(
        m_atmosphereSettings.mieScatter,
        m_atmosphereSettings.mieScatter,
        m_atmosphereSettings.mieScatter);
    const glm::vec3 mieExtinction(
        m_atmosphereSettings.mieExtinction,
        m_atmosphereSettings.mieExtinction,
        m_atmosphereSettings.mieExtinction);
    const glm::vec3 ozoneAbsorption(
        m_atmosphereSettings.ozoneAbsorptionR,
        m_atmosphereSettings.ozoneAbsorptionG,
        m_atmosphereSettings.ozoneAbsorptionB);
    const glm::vec3 hazeColor(
        m_atmosphereSettings.hazeColorR,
        m_atmosphereSettings.hazeColorG,
        m_atmosphereSettings.hazeColorB);
    const glm::vec3 ambientBlueTint(
        m_atmosphereSettings.ambientBlueTintR,
        m_atmosphereSettings.ambientBlueTintG,
        m_atmosphereSettings.ambientBlueTintB);
    const glm::vec3 sunsetTint(
        m_atmosphereSettings.sunsetTintR,
        m_atmosphereSettings.sunsetTintG,
        m_atmosphereSettings.sunsetTintB);

    constexpr std::size_t kTexelCount =
        static_cast<std::size_t>(kAtmosphereLutResolution) *
        static_cast<std::size_t>(kAtmosphereLutResolution) *
        static_cast<std::size_t>(kAtmosphereLutResolution);
    std::array<std::uint8_t, kTexelCount * 4> lutBytes{};

    for (std::uint32_t distanceIndex = 0; distanceIndex < kAtmosphereLutResolution; ++distanceIndex)
    {
        const float distanceT = static_cast<float>(distanceIndex) / static_cast<float>(kAtmosphereLutResolution - 1);
        const float physicalDistance = decodeLogDistance(distanceT, m_atmosphereSettings.atmosphereDistanceRange);
        const glm::vec3 viewOpticalDepth =
            (rayleighScattering * physicalDistance) +
            (mieExtinction * physicalDistance);
        const glm::vec3 viewTransmittance = expVec3(-viewOpticalDepth);
        const float pathFogAmount = 1.0f - std::exp(-physicalDistance / std::max(m_atmosphereSettings.pathFogDistance, 1.0f));
        const float longRangeHazeAmount = 1.0f - std::exp(-physicalDistance / std::max(m_atmosphereSettings.longRangeHazeDistance, 1.0f));

        for (std::uint32_t viewSunIndex = 0; viewSunIndex < kAtmosphereLutResolution; ++viewSunIndex)
        {
            const float viewSunT = static_cast<float>(viewSunIndex) / static_cast<float>(kAtmosphereLutResolution - 1);
            const float viewSunDot = (viewSunT * 2.0f) - 1.0f;
            const float rayleighPhaseValue = rayleighPhase(viewSunDot);
            const float miePhaseValue = henyeyGreensteinPhase(viewSunDot, m_atmosphereSettings.mieG);

            for (std::uint32_t timeIndex = 0; timeIndex < kAtmosphereLutResolution; ++timeIndex)
            {
                const float timeT = static_cast<float>(timeIndex) / static_cast<float>(kAtmosphereLutResolution);
                const float sunHeight = -std::sin(timeT * (glm::pi<float>() * 2.0f));
                const float sunVisibility = glm::smoothstep(-0.045f, 0.02f, sunHeight);
                const float daylight = glm::smoothstep(-0.12f, 0.10f, sunHeight);
                const float night = 1.0f - daylight;
                const float twilight = 1.0f - glm::smoothstep(0.02f, 0.22f, std::abs(sunHeight));
                const float airMass = approximateAirMass(sunHeight);

                const glm::vec3 sunOpticalDepth =
                    (rayleighScattering * (m_atmosphereSettings.rayleighScaleHeight * airMass)) +
                    (mieExtinction * (m_atmosphereSettings.mieScaleHeight * airMass)) +
                    (ozoneAbsorption * (m_atmosphereSettings.ozoneColumnHeight * airMass));
                const glm::vec3 sunTransmittance = expVec3(-sunOpticalDepth);
                const glm::vec3 incidentSunlight = sunTransmittance * sunVisibility;
                const glm::vec3 normalizedRayleighTint = glm::normalize(rayleighScattering);
                const glm::vec3 blueSkyTint = glm::mix(
                    ambientBlueTint,
                    normalizedRayleighTint * m_atmosphereSettings.rayleighTintScale,
                    m_atmosphereSettings.ambientBlueBias);
                const glm::vec3 ambientIlluminant = glm::mix(
                    blueSkyTint,
                    sunTransmittance,
                    m_atmosphereSettings.ambientSolarInfluence +
                    (m_atmosphereSettings.ambientTwilightInfluence * twilight)) * sunVisibility;
                const float aureole = std::pow(
                    saturate((viewSunDot + 1.0f) * 0.5f),
                    std::max(m_atmosphereSettings.aureolePower, 1.0f));
                const float sunDisk = std::pow(
                    saturate((viewSunDot + 1.0f) * 0.5f),
                    std::max(m_atmosphereSettings.sunDiskPower, 1.0f));
                const float sunwardGlow = std::pow(
                    saturate((viewSunDot + 1.0f) * 0.5f),
                    std::max(m_atmosphereSettings.sunGlowPower, 1.0f));

                const glm::vec3 scatteringCoefficient =
                    (rayleighScattering * rayleighPhaseValue) +
                    (mieScattering * miePhaseValue);
                const glm::vec3 sigmaT = rayleighScattering + mieExtinction;
                const glm::vec3 directInScatteredRadiance =
                    incidentSunlight *
                    scatteringCoefficient *
                    ((glm::vec3(1.0f) - viewTransmittance) / glm::max(sigmaT, glm::vec3(1.0e-6f)));
                const glm::vec3 ambientSkyRadiance =
                    ambientIlluminant *
                    rayleighScattering *
                    ((glm::vec3(1.0f) - viewTransmittance) / glm::max(rayleighScattering + mieScattering, glm::vec3(1.0e-6f))) *
                    m_atmosphereSettings.ambientSkyScale;
                const glm::vec3 sunsetRadiance =
                    sunsetTint *
                    twilight *
                    (m_atmosphereSettings.sunsetStrength + (m_atmosphereSettings.sunsetSunwardBoost * sunwardGlow)) *
                    (1.0f - night) *
                    (m_atmosphereSettings.sunsetDistanceMin +
                        (m_atmosphereSettings.sunsetDistanceMax * longRangeHazeAmount));
                const glm::vec3 hazeRadiance =
                    ambientIlluminant *
                    hazeColor *
                    longRangeHazeAmount * m_atmosphereSettings.hazeStrength;
                const glm::vec3 solarDiskRadiance =
                    incidentSunlight *
                    ((aureole * m_atmosphereSettings.aureoleStrength) +
                        (sunDisk * m_atmosphereSettings.sunDiskStrength));
                const glm::vec3 inScatteredRadiance =
                    directInScatteredRadiance +
                    ambientSkyRadiance +
                    hazeRadiance +
                    sunsetRadiance +
                    solarDiskRadiance;

                const glm::vec3 visibleColor = glm::vec3(1.0f) - expVec3(-inScatteredRadiance * m_atmosphereSettings.exposure);
                const float transmittanceLuma = luminance(viewTransmittance);
                const float alphaFromTransmittance = 1.0f - transmittanceLuma;
                float alpha = glm::max(alphaFromTransmittance * m_atmosphereSettings.alphaScale, pathFogAmount * daylight * 0.96f);
                alpha = glm::max(alpha, longRangeHazeAmount * daylight * 0.72f);
                alpha *= glm::mix(0.08f, 1.0f, daylight);
                alpha = glm::mix(alpha, alpha * 0.20f, night);
                alpha = saturate(alpha);

                const std::size_t texelIndex =
                    ((((static_cast<std::size_t>(distanceIndex) * kAtmosphereLutResolution) +
                       static_cast<std::size_t>(viewSunIndex)) * kAtmosphereLutResolution) +
                       static_cast<std::size_t>(timeIndex)) * 4u;
                const glm::vec3 encodedColor = saturate(visibleColor / std::max(alpha, 1.0e-4f));
                const float encodedAlpha = saturate(alpha);

                lutBytes[texelIndex + 0] = static_cast<std::uint8_t>(encodedColor.r * 255.0f);
                lutBytes[texelIndex + 1] = static_cast<std::uint8_t>(encodedColor.g * 255.0f);
                lutBytes[texelIndex + 2] = static_cast<std::uint8_t>(encodedColor.b * 255.0f);
                lutBytes[texelIndex + 3] = static_cast<std::uint8_t>(encodedAlpha * 255.0f);
            }
        }
    }

    return lutBytes;
}
