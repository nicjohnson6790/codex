#include "QuadtreeWaterMeshRenderer.hpp"
#include "PeriodicWorldPhase.hpp"

#include "AppConfig.hpp"
#include "PerformanceCapture.hpp"

#include <SDL3/SDL_stdinc.h>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cstddef>
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
constexpr std::uint32_t kWaterMeshIntervalCount = AppConfig::Water::kMeshVertexResolution - 1u;
constexpr std::uint32_t kWaterBaseMeshVertexResolution = AppConfig::Water::kMeshVertexResolution - 2u;
constexpr float kWaterMeshInset = 1.0f / static_cast<float>(kWaterMeshIntervalCount);
constexpr std::uint32_t kWaterBridgeOuterVertexCount = AppConfig::Water::kMeshVertexResolution;
constexpr std::uint32_t kWaterBridgeInnerVertexCount = kWaterMeshIntervalCount - 1u;
constexpr std::uint32_t kWaterCoarseBridgeOuterVertexCount = (kWaterMeshIntervalCount / 2u) + 1u;
constexpr std::uint32_t kWaterEqualBridgeQuadCount = kWaterBridgeInnerVertexCount - 1u;
constexpr std::uint32_t kFoamDetailTextureResolution = 256u;

struct GeneratedImage
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
    float decodeScale = 1.0f;
};

float fractf(float value)
{
    return value - std::floor(value);
}

float saturatef(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

float smoothstep01(float value)
{
    const float t = saturatef(value);
    return t * t * (3.0f - (2.0f * t));
}

float lerpf(float a, float b, float t)
{
    return a + ((b - a) * t);
}

float hash11(float x, float y, float seed)
{
    const float value = std::sin((x * 127.1f) + (y * 311.7f) + (seed * 74.7f)) * 43758.5453f;
    return fractf(value);
}

float periodicValueNoise(float x, float y, std::uint32_t grid, float seed)
{
    const float scaledX = x * static_cast<float>(grid);
    const float scaledY = y * static_cast<float>(grid);
    const float cellX = std::floor(scaledX);
    const float cellY = std::floor(scaledY);
    const std::uint32_t ix = static_cast<std::uint32_t>(cellX) % grid;
    const std::uint32_t iy = static_cast<std::uint32_t>(cellY) % grid;
    const std::uint32_t ix1 = (ix + 1u) % grid;
    const std::uint32_t iy1 = (iy + 1u) % grid;
    const float fx = smoothstep01(scaledX - cellX);
    const float fy = smoothstep01(scaledY - cellY);

    const float v00 = hash11(static_cast<float>(ix), static_cast<float>(iy), seed);
    const float v10 = hash11(static_cast<float>(ix1), static_cast<float>(iy), seed);
    const float v01 = hash11(static_cast<float>(ix), static_cast<float>(iy1), seed);
    const float v11 = hash11(static_cast<float>(ix1), static_cast<float>(iy1), seed);
    return lerpf(lerpf(v00, v10, fx), lerpf(v01, v11, fx), fy);
}

float periodicFbm(float x, float y, std::uint32_t octaves, std::uint32_t baseGrid, float seed)
{
    float amplitude = 1.0f;
    float total = 0.0f;
    float normalization = 0.0f;
    for (std::uint32_t octave = 0; octave < octaves; ++octave)
    {
        total += periodicValueNoise(x, y, baseGrid << octave, seed + static_cast<float>(octave * 17u)) * amplitude;
        normalization += amplitude;
        amplitude *= 0.5f;
    }

    return normalization > 0.0f ? (total / normalization) : 0.0f;
}

float wrapDistance(float a, float b)
{
    const float distance = std::fabs(a - b);
    return std::min(distance, 1.0f - distance);
}

std::pair<float, float> worleyF1F2(float x, float y, std::uint32_t cellCount, float seed)
{
    const float scaledX = x * static_cast<float>(cellCount);
    const float scaledY = y * static_cast<float>(cellCount);
    const int baseCellX = static_cast<int>(std::floor(scaledX));
    const int baseCellY = static_cast<int>(std::floor(scaledY));
    float f1 = 1.0e9f;
    float f2 = 1.0e9f;

    for (int oy = -1; oy <= 1; ++oy)
    {
        for (int ox = -1; ox <= 1; ++ox)
        {
            const std::uint32_t cx = static_cast<std::uint32_t>((baseCellX + ox + static_cast<int>(cellCount)) % static_cast<int>(cellCount));
            const std::uint32_t cy = static_cast<std::uint32_t>((baseCellY + oy + static_cast<int>(cellCount)) % static_cast<int>(cellCount));
            const float featureX = (static_cast<float>(cx) + hash11(static_cast<float>(cx), static_cast<float>(cy), seed)) / static_cast<float>(cellCount);
            const float featureY = (static_cast<float>(cy) + hash11(static_cast<float>(cy), static_cast<float>(cx), seed + 19.0f)) / static_cast<float>(cellCount);
            const float dx = wrapDistance(x, featureX);
            const float dy = wrapDistance(y, featureY);
            const float distanceSquared = (dx * dx) + (dy * dy);
            if (distanceSquared < f1)
            {
                f2 = f1;
                f1 = distanceSquared;
            }
            else if (distanceSquared < f2)
            {
                f2 = distanceSquared;
            }
        }
    }

    return { std::sqrt(f1), std::sqrt(f2) };
}

GeneratedImage buildFoamSdfTexture()
{
    GeneratedImage image{};
    image.width = kFoamDetailTextureResolution;
    image.height = kFoamDetailTextureResolution;
    image.pixels.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4u);
    std::vector<float> sdfValues(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height));
    float maxAbsSdf = 1.0e-5f;

    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        const float v = static_cast<float>(y) / static_cast<float>(image.height);
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const float u = static_cast<float>(x) / static_cast<float>(image.width);
            const auto [f1, f2] = worleyF1F2(u, v, 12u, 71.0f);
            const auto [g1, g2] = worleyF1F2(u + 0.17f, v - 0.09f, 23u, 163.0f);
            float sdf = (f2 - f1) - 0.052f;
            sdf += ((g2 - g1) - 0.024f) * 0.42f;
            sdf += (periodicFbm((u * 1.1f) + 0.23f, (v * 0.9f) - 0.11f, 4u, 4u, 241.0f) - 0.5f) * 0.026f;
            sdf += (periodicFbm((u * 2.0f) - 0.08f, (v * 1.8f) + 0.14f, 3u, 7u, 509.0f) - 0.5f) * 0.013f;

            const std::size_t sampleIndex = static_cast<std::size_t>(y) * image.width + x;
            sdfValues[sampleIndex] = sdf;
            maxAbsSdf = std::max(maxAbsSdf, std::fabs(sdf));
        }
    }

    const float encodeScale = 0.48f / maxAbsSdf;
    image.decodeScale = 1.0f / encodeScale;

    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const std::size_t sampleIndex = static_cast<std::size_t>(y) * image.width + x;
            const float encoded = saturatef(0.5f + (sdfValues[sampleIndex] * encodeScale));
            const std::uint8_t channel = static_cast<std::uint8_t>(std::round(encoded * 255.0f));
            const std::size_t pixelIndex = sampleIndex * 4u;
            image.pixels[pixelIndex + 0u] = channel;
            image.pixels[pixelIndex + 1u] = channel;
            image.pixels[pixelIndex + 2u] = channel;
            image.pixels[pixelIndex + 3u] = 255u;
        }
    }

    return image;
}

GeneratedImage buildFoamNoiseTexture()
{
    GeneratedImage image{};
    image.width = kFoamDetailTextureResolution;
    image.height = kFoamDetailTextureResolution;
    image.pixels.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4u);

    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        const float v = static_cast<float>(y) / static_cast<float>(image.height);
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const float u = static_cast<float>(x) / static_cast<float>(image.width);
            float noiseR = periodicFbm(u, v, 5u, 5u, 811.0f);
            noiseR = lerpf(noiseR, periodicFbm((u * 1.7f) + 0.19f, (v * 1.4f) - 0.13f, 3u, 9u, 977.0f), 0.35f);
            noiseR = smoothstep01((noiseR - 0.18f) / 0.68f);

            float noiseG = periodicFbm((u * 0.93f) + 0.27f, (v * 1.11f) - 0.21f, 5u, 6u, 1231.0f);
            noiseG = lerpf(noiseG, periodicFbm((u * 1.41f) - 0.16f, (v * 1.62f) + 0.08f, 3u, 10u, 1597.0f), 0.32f);
            noiseG = smoothstep01((noiseG - 0.18f) / 0.68f);

            float noiseB = periodicFbm((u * 1.23f) - 0.11f, (v * 0.87f) + 0.29f, 4u, 7u, 1877.0f);
            noiseB = lerpf(noiseB, periodicFbm((u * 2.03f) + 0.05f, (v * 1.33f) - 0.24f, 2u, 12u, 2137.0f), 0.28f);
            noiseB = smoothstep01((noiseB - 0.18f) / 0.68f);

            float noiseA = periodicFbm((u * 0.78f) + 0.34f, (v * 1.52f) + 0.17f, 4u, 8u, 2459.0f);
            noiseA = lerpf(noiseA, periodicFbm((u * 1.84f) - 0.27f, (v * 1.08f) + 0.31f, 3u, 11u, 2767.0f), 0.30f);
            noiseA = smoothstep01((noiseA - 0.18f) / 0.68f);

            const std::uint8_t channelR = static_cast<std::uint8_t>(std::round(saturatef(noiseR) * 255.0f));
            const std::uint8_t channelG = static_cast<std::uint8_t>(std::round(saturatef(noiseG) * 255.0f));
            const std::uint8_t channelB = static_cast<std::uint8_t>(std::round(saturatef(noiseB) * 255.0f));
            const std::uint8_t channelA = static_cast<std::uint8_t>(std::round(saturatef(noiseA) * 255.0f));
            const std::size_t pixelIndex =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + x) * 4u;
            image.pixels[pixelIndex + 0u] = channelR;
            image.pixels[pixelIndex + 1u] = channelG;
            image.pixels[pixelIndex + 2u] = channelB;
            image.pixels[pixelIndex + 3u] = channelA;
        }
    }

    return image;
}

float normalizedWaterCoord(std::uint32_t index)
{
    return static_cast<float>(index) / static_cast<float>(kWaterMeshIntervalCount);
}

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
    const bool foamEnabled = settings.crestFoamEnabled && settings.drawFoam;
    return glm::vec4(
        foamEnabled ? std::max(settings.crestFoamAmount, 0.0f) : 0.0f,
        std::max(settings.crestFoamThreshold, 0.0f),
        std::max(settings.crestFoamSoftness, 1.0e-4f),
        std::max(settings.crestFoamSlopeStart, 0.0f));
}

glm::vec4 buildFoamHistoryParams(const WaterSettings& settings, bool hasValidFoamHistory)
{
    const bool drawFoam = settings.drawFoam;
    return glm::vec4(
        drawFoam ? std::max(settings.crestFoamDecayRate, 0.0f) : 0.0f,
        (drawFoam && hasValidFoamHistory) ? 1.0f : 0.0f,
        drawFoam ? 1.0f : 0.0f,
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
    SDL_GPUBufferCreateInfo descriptorInfo{};
    descriptorInfo.usage=SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    descriptorInfo.size=sizeof(m_descriptors);
    m_descriptorBuffer=SDL_CreateGPUBuffer(m_device,&descriptorInfo);
    SDL_GPUTransferBufferCreateInfo staging{};
    staging.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    staging.size=descriptorInfo.size;
    m_descriptorTransferBuffer=SDL_CreateGPUTransferBuffer(m_device,&staging);
    SDL_GPUBufferCreateInfo bodyInfo{};
    bodyInfo.usage=SDL_GPU_BUFFERUSAGE_INDIRECT;
    bodyInfo.size=sizeof(SDL_GPUIndexedIndirectDrawCommand);
    m_indirectBuffer=SDL_CreateGPUBuffer(m_device,&bodyInfo);
    staging.size=bodyInfo.size;
    m_indirectTransferBuffer=SDL_CreateGPUTransferBuffer(m_device,&staging);
    if (!m_descriptorBuffer || !m_descriptorTransferBuffer || !m_indirectBuffer || !m_indirectTransferBuffer)
        throwSdlError("Failed to create water descriptor resources.");
    createMesh();
    createWorkingBuffers();
    createWaterTextures();
    createFoamDetailTextures();
    createWaterSampler();
    createPipelines(shaderDirectory);
    createWaterComputePipelines(shaderDirectory);

    SDL_GPUBufferCreateInfo bridgeIndirectInfo{};
    bridgeIndirectInfo.usage = SDL_GPU_BUFFERUSAGE_INDIRECT;
    bridgeIndirectInfo.size = sizeof(SDL_GPUIndexedIndirectDrawCommand) * static_cast<Uint32>(m_bridgeIndirectCommands.size());
    m_bridgeIndirectBuffer = SDL_CreateGPUBuffer(m_device, &bridgeIndirectInfo);
    if (m_bridgeIndirectBuffer == nullptr)
    {
        throwSdlError("Failed to create water bridge indirect buffer.");
    }

    SDL_GPUTransferBufferCreateInfo bridgeIndirectTransferInfo{};
    bridgeIndirectTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    bridgeIndirectTransferInfo.size = bridgeIndirectInfo.size;
    m_bridgeIndirectTransferBuffer = SDL_CreateGPUTransferBuffer(m_device, &bridgeIndirectTransferInfo);
    if (m_bridgeIndirectTransferBuffer == nullptr)
    {
        throwSdlError("Failed to create water bridge indirect transfer buffer.");
    }
}

void QuadtreeWaterMeshRenderer::shutdown()
{
    if (m_descriptorBuffer) SDL_ReleaseGPUBuffer(m_device,m_descriptorBuffer);
    if (m_indirectBuffer) SDL_ReleaseGPUBuffer(m_device,m_indirectBuffer);
    if (m_descriptorTransferBuffer) SDL_ReleaseGPUTransferBuffer(m_device,m_descriptorTransferBuffer);
    if (m_indirectTransferBuffer) SDL_ReleaseGPUTransferBuffer(m_device,m_indirectTransferBuffer);
    m_descriptorBuffer=nullptr; m_indirectBuffer=nullptr;
    m_descriptorTransferBuffer=nullptr; m_indirectTransferBuffer=nullptr;
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
    if (m_bridgePipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_bridgePipeline);
        m_bridgePipeline = nullptr;
    }
    if (m_mainPipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_mainPipeline);
        m_mainPipeline = nullptr;
    }
    if (m_bridgeIndirectTransferBuffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, m_bridgeIndirectTransferBuffer);
        m_bridgeIndirectTransferBuffer = nullptr;
    }
    if (m_bridgeIndirectBuffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, m_bridgeIndirectBuffer);
        m_bridgeIndirectBuffer = nullptr;
    }

    destroyWaterSampler();
    destroyFoamDetailTextures();
    destroyWaterTextures();
    destroyWorkingBuffers();
    destroyMesh();
    clear();
}

void QuadtreeWaterMeshRenderer::clear()
{
    m_instanceCount = 0;
    m_bridgeIndirectCommands = {};
    m_bridgeIndirectCommandCount = 0;
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
    std::uint32_t bandMask, std::uint32_t bridgeMask, std::uint32_t coarseBridgeMask)
{
    (void)leafId;

    if (m_instanceCount >= AppConfig::Water::kMaxWaterInstances)
    {
        return;
    }

    const glm::vec3 localOrigin = glm::vec3(surfacePositionRelativeTo(leafOrigin, m_activeCameraPosition));
    ParentDescriptor& parent = m_descriptors.parents[m_instanceCount++];
    parent.edges = glm::uvec4(bridgeMask, coarseBridgeMask, 0u, 0u);
    InstanceData& instance = parent.body;
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
    const auto upload = [&](SDL_GPUTransferBuffer* staging, SDL_GPUBuffer* buffer, const void* data, Uint32 bytes) {
        void* mapped = SDL_MapGPUTransferBuffer(m_device, staging, true);
        if (!mapped) throwSdlError("Failed to upload water draw descriptors.");
        std::memcpy(mapped, data, bytes);
        SDL_UnmapGPUTransferBuffer(m_device, staging);
        SDL_GPUTransferBufferLocation source{staging, 0};
        SDL_GPUBufferRegion destination{buffer, 0, bytes};
        SDL_UploadToGPUBuffer(copyPass, &source, &destination, true);
    };
    const auto counts = m_descriptors.buildBridges(m_instanceCount, [](const auto& parent, std::uint32_t edge) {
        const auto bit = 1u << edge;
        return (parent.edges.y & bit) ? 1 : (parent.edges.x & bit) ? 0 : -1;
    });
    if (m_instanceCount)
        upload(m_descriptorTransferBuffer, m_descriptorBuffer, &m_descriptors,
            Uint32(offsetof(Descriptors, bridges) + sizeof(std::uint32_t) * (counts.normal + counts.coarse)));
    SDL_GPUIndexedIndirectDrawCommand body{m_mesh.indexCount,m_instanceCount,0,0,0};
    upload(m_indirectTransferBuffer,m_indirectBuffer,&body,sizeof(body));
    m_bridgeIndirectCommands[0]={m_bridgeMeshRange.indexCount,counts.normal,m_bridgeMeshRange.firstIndex,0,0};
    m_bridgeIndirectCommands[1]={m_coarseBridgeMeshRange.indexCount,counts.coarse,m_coarseBridgeMeshRange.firstIndex,0,
        counts.normal};
    upload(m_bridgeIndirectTransferBuffer,m_bridgeIndirectBuffer,m_bridgeIndirectCommands.data(),sizeof(m_bridgeIndirectCommands));
    m_bridgeIndirectCommandCount=2;
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
    Extent2D viewportExtent,
    float timeSeconds,
    SDL_GPUBuffer* terrainHeightmapBuffer,
    const CloudRenderer::SamplingResources& clouds) const
{
    HELLO_PROFILE_SCOPE_GROUPS("QuadtreeWaterMeshRenderer::Render", ProfileScopeGroup::Renderer);

    if (!m_settings.enabled ||
        totalInstanceCount() == 0 ||
        m_mainPipeline == nullptr ||
        m_bridgePipeline == nullptr ||
        m_displacementTexture == nullptr ||
        m_slopeTexture == nullptr ||
        m_foamHistoryReadTexture == nullptr ||
        m_foamDetailSdfTexture == nullptr ||
        m_foamDetailNoiseTexture == nullptr ||
        m_waterSampler == nullptr ||
        skyboxRenderer.cubemapTexture() == nullptr ||
        skyboxRenderer.cubemapSampler() == nullptr ||
        terrainHeightmapBuffer == nullptr)
    {
        return;
    }

    const WaterUniforms uniforms = buildWaterUniforms(
        viewProjection,
        lightingSystem,
        skyboxRenderer,
        viewportExtent,
        timeSeconds);

    const auto drawMeshInstances = [&](SDL_GPUGraphicsPipeline* pipeline, const MeshResources& mesh)
    {
        if (pipeline == nullptr ||
            mesh.vertexBuffer == nullptr ||
            mesh.indexBuffer == nullptr ||
            m_descriptorBuffer == nullptr ||
            m_instanceCount == 0)
        {
            return;
        }

        SDL_BindGPUGraphicsPipeline(renderPass, pipeline);
        SDL_PushGPUVertexUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));
        SDL_PushGPUFragmentUniformData(commandBuffer, 1, &clouds.state, sizeof(clouds.state));

        const SDL_GPUTextureSamplerBinding vertexSamplerBindings[1]{
            { m_displacementTexture, m_waterSampler },
        };
        SDL_BindGPUVertexSamplers(renderPass, 0, vertexSamplerBindings, 1);

        const SDL_GPUTextureSamplerBinding fragmentSamplerBindings[8]{
            { m_displacementTexture, m_waterSampler },
            { m_slopeTexture, m_waterSampler },
            { m_foamHistoryReadTexture, m_waterSampler },
            { skyboxRenderer.cubemapTexture(), skyboxRenderer.cubemapSampler() },
            { m_foamDetailSdfTexture, m_waterSampler },
            { m_foamDetailNoiseTexture, m_waterSampler },
            clouds.coverage, clouds.noise,
        };
        SDL_BindGPUFragmentSamplers(renderPass, 0, fragmentSamplerBindings, 8);

        const SDL_GPUBufferBinding vertexBinding{ mesh.vertexBuffer, 0 };
        SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

        const SDL_GPUBufferBinding indexBinding{ mesh.indexBuffer, 0 };
        SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        SDL_GPUBuffer* storageBuffers[]{ terrainHeightmapBuffer, m_descriptorBuffer };
        SDL_BindGPUVertexStorageBuffers(renderPass, 0, storageBuffers, 2);
        SDL_DrawGPUIndexedPrimitivesIndirect(renderPass, m_indirectBuffer, 0, 1);
    };

    drawMeshInstances(m_mainPipeline, m_mesh);
    if (m_bridgeIndirectCommandCount > 0 &&
        m_bridgeMesh.vertexBuffer != nullptr &&
        m_bridgeMesh.indexBuffer != nullptr &&
        m_descriptorBuffer != nullptr &&
        m_bridgeIndirectBuffer != nullptr)
    {
        SDL_BindGPUGraphicsPipeline(renderPass, m_bridgePipeline);
        SDL_PushGPUVertexUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));
        SDL_PushGPUFragmentUniformData(commandBuffer, 0, &uniforms, sizeof(uniforms));
        SDL_PushGPUFragmentUniformData(commandBuffer, 1, &clouds.state, sizeof(clouds.state));

        const SDL_GPUTextureSamplerBinding vertexSamplerBindings[1]{
            { m_displacementTexture, m_waterSampler },
        };
        SDL_BindGPUVertexSamplers(renderPass, 0, vertexSamplerBindings, 1);

        const SDL_GPUTextureSamplerBinding fragmentSamplerBindings[8]{
            { m_displacementTexture, m_waterSampler },
            { m_slopeTexture, m_waterSampler },
            { m_foamHistoryReadTexture, m_waterSampler },
            { skyboxRenderer.cubemapTexture(), skyboxRenderer.cubemapSampler() },
            { m_foamDetailSdfTexture, m_waterSampler },
            { m_foamDetailNoiseTexture, m_waterSampler },
            clouds.coverage, clouds.noise,
        };
        SDL_BindGPUFragmentSamplers(renderPass, 0, fragmentSamplerBindings, 8);

        const SDL_GPUBufferBinding vertexBinding{ m_bridgeMesh.vertexBuffer, 0 };
        SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);

        const SDL_GPUBufferBinding indexBinding{ m_bridgeMesh.indexBuffer, 0 };
        SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        SDL_GPUBuffer* storageBuffers[]{ terrainHeightmapBuffer, m_descriptorBuffer };
        SDL_BindGPUVertexStorageBuffers(renderPass, 0, storageBuffers, 2);
        SDL_DrawGPUIndexedPrimitivesIndirect(renderPass, m_bridgeIndirectBuffer, 0, m_bridgeIndirectCommandCount);
    }
}

std::uint32_t QuadtreeWaterMeshRenderer::instanceCount() const
{
    return m_instanceCount;
}

std::uint32_t QuadtreeWaterMeshRenderer::totalInstanceCount() const
{
    return m_instanceCount + m_bridgeIndirectCommands[0].num_instances + m_bridgeIndirectCommands[1].num_instances;
}

std::uint32_t QuadtreeWaterMeshRenderer::packMetadata(
    std::uint8_t quadtreeLodHint,
    std::uint32_t bandMask,
    std::uint8_t edgeIndex)
{
    return
        (static_cast<std::uint32_t>(quadtreeLodHint) & 0xFFu) |
        ((static_cast<std::uint32_t>(edgeIndex) & 0x3u) << 8u) |
        ((bandMask & 0xFFFFu) << 16);
}

void QuadtreeWaterMeshRenderer::createPipelines(const std::filesystem::path& shaderDirectory)
{
    SDL_GPUVertexBufferDescription vertexBufferDescription{};
    vertexBufferDescription.slot = 0;
    vertexBufferDescription.pitch = sizeof(Vertex);
    vertexBufferDescription.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute vertexAttribute{};
    vertexAttribute.location = 0;
    vertexAttribute.buffer_slot = 0;
    vertexAttribute.format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    vertexAttribute.offset = offsetof(Vertex, localCoord);

    SDL_GPUColorTargetBlendState blendState{};
    blendState.enable_blend = true;
    blendState.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blendState.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blendState.color_blend_op = SDL_GPU_BLENDOP_ADD;
    blendState.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blendState.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blendState.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    blendState.color_write_mask =
        SDL_GPU_COLORCOMPONENT_R |
        SDL_GPU_COLORCOMPONENT_G |
        SDL_GPU_COLORCOMPONENT_B |
        SDL_GPU_COLORCOMPONENT_A;

    SDL_GPUColorTargetDescription colorTargetDescription{};
    colorTargetDescription.format = m_colorFormat;
    colorTargetDescription.blend_state = blendState;

    SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
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
    pipelineInfo.vertex_input_state.num_vertex_attributes = 1;
    pipelineInfo.vertex_input_state.vertex_attributes = &vertexAttribute;

    auto createGraphicsPipeline = [this, &pipelineInfo, &shaderDirectory](const char* vertexShaderName)
    {
        SDL_GPUShader* vertexShader = createShader(
            shaderDirectory / vertexShaderName,
            SDL_GPU_SHADERSTAGE_VERTEX,
            1,
            2,
            1);
        SDL_GPUShader* fragmentShader = createShader(
            shaderDirectory / "water_mesh.frag.spv",
            SDL_GPU_SHADERSTAGE_FRAGMENT,
            2,
            0,
            8);
        pipelineInfo.vertex_shader = vertexShader;
        pipelineInfo.fragment_shader = fragmentShader;
        SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
        SDL_ReleaseGPUShader(m_device, fragmentShader);
        SDL_ReleaseGPUShader(m_device, vertexShader);
        return pipeline;
    };

    m_mainPipeline = createGraphicsPipeline("water_mesh.vert.spv");
    if (m_mainPipeline == nullptr)
    {
        throwSdlError("Failed to create water graphics pipeline.");
    }

    m_bridgePipeline = createGraphicsPipeline("water_mesh_bridge.vert.spv");
    if (m_bridgePipeline == nullptr)
    {
        throwSdlError("Failed to create water bridge graphics pipeline.");
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
    createMeshGeometry(kWaterBaseMeshVertexResolution);

    std::vector<Vertex> bridgeVertices;
    bridgeVertices.reserve(kWaterBridgeOuterVertexCount + kWaterBridgeInnerVertexCount);
    std::array<std::uint32_t, kWaterBridgeOuterVertexCount> outerVertexIndices{};
    std::array<std::uint32_t, kWaterBridgeInnerVertexCount> innerVertexIndices{};
    const auto appendBridgeVertex = [&bridgeVertices](float x, float y)
    {
        const std::uint32_t vertexIndex = static_cast<std::uint32_t>(bridgeVertices.size());
        bridgeVertices.push_back(Vertex{ { x, y } });
        return vertexIndex;
    };

    outerVertexIndices[0] = appendBridgeVertex(0.0f, normalizedWaterCoord(0u));

    for (std::uint32_t y = 1; y < kWaterMeshIntervalCount; ++y)
    {
        outerVertexIndices[y] = appendBridgeVertex(0.0f, normalizedWaterCoord(y));
    }

    outerVertexIndices[kWaterBridgeOuterVertexCount - 1u] = appendBridgeVertex(0.0f, normalizedWaterCoord(kWaterMeshIntervalCount));

    for (std::uint32_t y = 1; y < kWaterMeshIntervalCount; ++y)
    {
        innerVertexIndices[y - 1u] = appendBridgeVertex(kWaterMeshInset, normalizedWaterCoord(y));
    }

    std::vector<std::uint32_t> bridgeIndices;
    bridgeIndices.reserve((kWaterEqualBridgeQuadCount * 6u) + 6u);

    bridgeIndices.push_back(outerVertexIndices[0]);
    bridgeIndices.push_back(outerVertexIndices[1]);
    bridgeIndices.push_back(innerVertexIndices[0]);

    for (std::uint32_t y = 0; y < kWaterEqualBridgeQuadCount; ++y)
    {
        const std::uint32_t outerTop = outerVertexIndices[y + 1u];
        const std::uint32_t outerBottom = outerVertexIndices[y + 2u];
        const std::uint32_t innerTop = innerVertexIndices[y];
        const std::uint32_t innerBottom = innerVertexIndices[y + 1u];

        bridgeIndices.push_back(outerTop);
        bridgeIndices.push_back(outerBottom);
        bridgeIndices.push_back(innerTop);

        bridgeIndices.push_back(innerTop);
        bridgeIndices.push_back(outerBottom);
        bridgeIndices.push_back(innerBottom);
    }

    bridgeIndices.push_back(outerVertexIndices[kWaterBridgeOuterVertexCount - 2u]);
    bridgeIndices.push_back(outerVertexIndices[kWaterBridgeOuterVertexCount - 1u]);
    bridgeIndices.push_back(innerVertexIndices[kWaterBridgeInnerVertexCount - 1u]);

    std::vector<Vertex> coarseBridgeVertices;
    coarseBridgeVertices.reserve(kWaterCoarseBridgeOuterVertexCount + kWaterBridgeInnerVertexCount);
    std::array<std::uint32_t, kWaterCoarseBridgeOuterVertexCount> coarseOuterVertexIndices{};
    std::array<std::uint32_t, kWaterBridgeInnerVertexCount> coarseInnerVertexIndices{};

    for (std::uint32_t y = 0; y < kWaterCoarseBridgeOuterVertexCount; ++y)
    {
        coarseOuterVertexIndices[y] = static_cast<std::uint32_t>(coarseBridgeVertices.size());
        coarseBridgeVertices.push_back(Vertex{ { 0.0f, normalizedWaterCoord(y * 2u) } });
    }

    for (std::uint32_t innerIndex = 0; innerIndex < kWaterBridgeInnerVertexCount; ++innerIndex)
    {
        coarseInnerVertexIndices[innerIndex] = static_cast<std::uint32_t>(coarseBridgeVertices.size());
        coarseBridgeVertices.push_back(bridgeVertices[kWaterBridgeOuterVertexCount + innerIndex]);
    }

    std::vector<std::uint32_t> coarseBridgeIndices;
    coarseBridgeIndices.reserve((126u * 9u) + 12u);

    coarseBridgeIndices.push_back(coarseOuterVertexIndices[0]);
    coarseBridgeIndices.push_back(coarseOuterVertexIndices[1]);
    coarseBridgeIndices.push_back(coarseInnerVertexIndices[0]);

    coarseBridgeIndices.push_back(coarseInnerVertexIndices[0]);
    coarseBridgeIndices.push_back(coarseOuterVertexIndices[1]);
    coarseBridgeIndices.push_back(coarseInnerVertexIndices[1]);

    for (std::uint32_t coarseSegment = 1; coarseSegment < (kWaterCoarseBridgeOuterVertexCount - 2u); ++coarseSegment)
    {
        const std::uint32_t outerStart = coarseOuterVertexIndices[coarseSegment];
        const std::uint32_t outerEnd = coarseOuterVertexIndices[coarseSegment + 1u];
        const std::uint32_t innerStart = coarseInnerVertexIndices[(coarseSegment * 2u) - 1u];
        const std::uint32_t innerMid = coarseInnerVertexIndices[coarseSegment * 2u];
        const std::uint32_t innerEnd = coarseInnerVertexIndices[(coarseSegment * 2u) + 1u];

        coarseBridgeIndices.push_back(outerStart);
        coarseBridgeIndices.push_back(outerEnd);
        coarseBridgeIndices.push_back(innerMid);

        coarseBridgeIndices.push_back(innerMid);
        coarseBridgeIndices.push_back(outerEnd);
        coarseBridgeIndices.push_back(innerEnd);

        coarseBridgeIndices.push_back(outerStart);
        coarseBridgeIndices.push_back(innerMid);
        coarseBridgeIndices.push_back(innerStart);
    }

    coarseBridgeIndices.push_back(coarseOuterVertexIndices[kWaterCoarseBridgeOuterVertexCount - 2u]);
    coarseBridgeIndices.push_back(coarseOuterVertexIndices[kWaterCoarseBridgeOuterVertexCount - 1u]);
    coarseBridgeIndices.push_back(coarseInnerVertexIndices[kWaterBridgeInnerVertexCount - 1u]);

    coarseBridgeIndices.push_back(coarseOuterVertexIndices[kWaterCoarseBridgeOuterVertexCount - 2u]);
    coarseBridgeIndices.push_back(coarseInnerVertexIndices[kWaterBridgeInnerVertexCount - 1u]);
    coarseBridgeIndices.push_back(coarseInnerVertexIndices[kWaterBridgeInnerVertexCount - 2u]);

    std::vector<Vertex> combinedBridgeVertices = std::move(bridgeVertices);
    std::vector<std::uint32_t> combinedBridgeIndices = std::move(bridgeIndices);
    m_bridgeMeshRange.firstIndex = 0;
    m_bridgeMeshRange.indexCount = static_cast<std::uint32_t>(combinedBridgeIndices.size());

    const std::uint32_t coarseVertexBase = static_cast<std::uint32_t>(combinedBridgeVertices.size());
    combinedBridgeVertices.insert(
        combinedBridgeVertices.end(),
        coarseBridgeVertices.begin(),
        coarseBridgeVertices.end());
    m_coarseBridgeMeshRange.firstIndex = static_cast<std::uint32_t>(combinedBridgeIndices.size());
    m_coarseBridgeMeshRange.indexCount = static_cast<std::uint32_t>(coarseBridgeIndices.size());
    for (std::uint32_t& index : coarseBridgeIndices)
    {
        index += coarseVertexBase;
    }
    combinedBridgeIndices.insert(
        combinedBridgeIndices.end(),
        coarseBridgeIndices.begin(),
        coarseBridgeIndices.end());

    createMeshResources(combinedBridgeVertices, combinedBridgeIndices, m_bridgeMesh);
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
            vertices.push_back(Vertex{
                { normalizedWaterCoord(x + 1u), normalizedWaterCoord(y + 1u) }
            });
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

    createMeshResources(vertices, indices, resources);
}

void QuadtreeWaterMeshRenderer::destroyMesh()
{
    auto destroyMeshResources = [this](MeshResources& resources)
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
    };

    destroyMeshResources(m_bridgeMesh);
    destroyMeshResources(m_mesh);
    m_bridgeMeshRange = {};
    m_coarseBridgeMeshRange = {};
}

void QuadtreeWaterMeshRenderer::createMeshResources(
    const std::vector<Vertex>& vertices,
    const std::vector<std::uint32_t>& indices,
    MeshResources& resources)
{
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

void QuadtreeWaterMeshRenderer::createFoamDetailTextures()
{
    const std::array<GeneratedImage, 2> images{
        buildFoamSdfTexture(),
        buildFoamNoiseTexture(),
    };
    std::array<SDL_GPUTexture**, 2> textureSlots{
        &m_foamDetailSdfTexture,
        &m_foamDetailNoiseTexture,
    };

    for (std::size_t textureIndex = 0; textureIndex < images.size(); ++textureIndex)
    {
        const GeneratedImage& image = images[textureIndex];
        if (image.width == 0 || image.height == 0)
        {
            throw std::runtime_error("Generated water foam detail textures must be non-empty.");
        }

        SDL_GPUTextureCreateInfo textureInfo{};
        textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
        textureInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        textureInfo.width = image.width;
        textureInfo.height = image.height;
        textureInfo.layer_count_or_depth = 1;
        textureInfo.num_levels = 1;
        textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;

        SDL_GPUTexture* texture = SDL_CreateGPUTexture(m_device, &textureInfo);
        if (texture == nullptr)
        {
            throwSdlError("Failed to create foam detail texture.");
        }

        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = static_cast<Uint32>(image.pixels.size());
        SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
        if (transferBuffer == nullptr)
        {
            SDL_ReleaseGPUTexture(m_device, texture);
            throwSdlError("Failed to create foam detail upload transfer buffer.");
        }

        void* mapped = SDL_MapGPUTransferBuffer(m_device, transferBuffer, false);
        std::memcpy(mapped, image.pixels.data(), image.pixels.size());
        SDL_UnmapGPUTransferBuffer(m_device, transferBuffer);

        SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
        SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);

        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = transferBuffer;
        source.offset = 0;
        source.pixels_per_row = image.width;
        source.rows_per_layer = image.height;

        SDL_GPUTextureRegion destination{};
        destination.texture = texture;
        destination.w = image.width;
        destination.h = image.height;
        destination.d = 1;
        SDL_UploadToGPUTexture(copyPass, &source, &destination, false);

        SDL_EndGPUCopyPass(copyPass);
        if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
        {
            SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
            SDL_ReleaseGPUTexture(m_device, texture);
            throwSdlError("Failed to upload foam detail texture.");
        }

        SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
        *textureSlots[textureIndex] = texture;
    }

    m_foamDetailSdfDecodeScale = images[0].decodeScale;
}

void QuadtreeWaterMeshRenderer::destroyFoamDetailTextures()
{
    m_foamDetailSdfDecodeScale = 1.0f;
    if (m_foamDetailNoiseTexture != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_foamDetailNoiseTexture);
        m_foamDetailNoiseTexture = nullptr;
    }
    if (m_foamDetailSdfTexture != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_foamDetailSdfTexture);
        m_foamDetailSdfTexture = nullptr;
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
    Extent2D viewportExtent,
    float timeSeconds) const
{
    WaterUniforms uniforms{};
    uniforms.viewProjection = viewProjection;

    uniforms.cameraAndTime = glm::vec4(
        0.0f,
        0.0f,
        static_cast<float>(m_activeCameraPosition.localPosition().y),
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
    uniforms.atmosphereOptics = skyboxRenderer.buildAtmosphereOptics(lightingSystem);
    const auto& medium = skyboxRenderer.waterMediumSettings();
    uniforms.waterAbsorption = glm::vec4(medium.absorption, 0.0f);
    uniforms.waterScattering = glm::vec4(medium.scattering, medium.sourceScale);
    uniforms.skyRotation = sharedSkyUniforms.skyRotation;
    uniforms.atmosphereParams = sharedSkyUniforms.atmosphereParams;
    uniforms.sunDirectionTimeOfDay = sharedSkyUniforms.sunDirectionTimeOfDay;
    uniforms.opticalParams = glm::vec4(
        0.0f, // Reserved; shader derives dielectric F0 from its fixed water IOR.
        AppConfig::Water::kBaseRoughness,
        AppConfig::Water::kSlopeRoughnessStrength,
        AppConfig::Water::kEnvironmentReflectionStrength);
    uniforms.refractionParams = glm::vec4(
        AppConfig::Water::kShallowRefractionMaxDepthMeters,
        AppConfig::Water::kShallowRefractionFullFadeDepthMeters,
        0.0f,
        0.0f);
    uniforms.distanceLodParams = glm::vec4(
        static_cast<float>(std::max(viewportExtent.height, 1u)),
        std::tan(AppConfig::Camera::kVerticalFovRadians * 0.5f),
        AppConfig::Water::kFarNormalFadeStartMeters,
        AppConfig::Water::kFarNormalFadeEndMeters);
    uniforms.cascadeFilterParams = glm::vec4(
        AppConfig::Water::kCascadeDetailTexelFadeStart,
        AppConfig::Water::kCascadeDetailTexelFadeEnd,
        AppConfig::Water::kFarRoughnessFadeStartMeters,
        AppConfig::Water::kFarRoughnessFadeEndMeters);
    uniforms.farFieldParams = glm::vec4(
        AppConfig::Water::kFarRoughnessBoost,
        AppConfig::Water::kFarReflectionFlattenStartMeters,
        AppConfig::Water::kFarReflectionFlattenEndMeters,
        AppConfig::Water::kFoamRoughness);
    uniforms.foamLodParams = glm::vec4(
        AppConfig::Water::kFarFoamFadeStartMeters,
        AppConfig::Water::kFarFoamFadeEndMeters,
        AppConfig::Water::kFoamCascadeDetailTexelThreshold,
        0.0f);
    uniforms.foamParams = buildFoamGenerationParams(m_settings);
    uniforms.foamParams2 = buildFoamHistoryParams(m_settings, false);
    uniforms.foamColor = glm::vec4(
        AppConfig::Water::kCrestFoamColor,
        std::max(m_settings.crestFoamBrightness, 0.0f));
    const float foamRidgeMinA = std::max(m_settings.foamSdfRidgeMinA, 0.0f);
    const float foamRidgeMinB = std::max(m_settings.foamSdfRidgeMinB, 0.0f);
    const float foamEvolutionStart = std::min(m_settings.foamEvolutionStart, m_settings.foamEvolutionEnd);
    const float foamEvolutionEnd = std::max(m_settings.foamEvolutionStart, m_settings.foamEvolutionEnd);
    const float foamEvolutionDropoffEnd = std::max(m_settings.foamEvolutionDropoffEnd, foamEvolutionEnd + 1.0e-4f);
    const float foamFadeStart = std::min(m_settings.foamFadeStart, m_settings.foamFadeEnd);
    const float foamFadeEnd = std::max(m_settings.foamFadeStart, m_settings.foamFadeEnd);
    uniforms.foamDetailShape = glm::vec4(
        std::max(m_settings.foamSdfSampleScaleA, 0.0001f),
        m_foamDetailSdfDecodeScale,
        std::max(m_settings.foamNoiseScale, 0.0001f),
        std::max(m_settings.foamHistoryWarpStrength, 0.0f));
    uniforms.foamDetailRidges = glm::vec4(
        foamRidgeMinA,
        std::max(m_settings.foamSdfRidgeMaxA, foamRidgeMinA + 1.0e-4f),
        foamRidgeMinB,
        std::max(m_settings.foamSdfRidgeMaxB, foamRidgeMinB + 1.0e-4f));
    uniforms.foamDetailBreakup = glm::vec4(
        std::max(m_settings.foamDetailOffsetStrength, 0.0f),
        std::max(m_settings.foamDetailBreakupScale, 0.0001f),
        std::max(m_settings.foamDetailBreakupStrength, 0.0f),
        0.0f);
    const auto storePhase = [](glm::vec4& packed, std::uint32_t index, const glm::dvec2& phase) {
        (&packed.x)[index * 2u] = static_cast<float>(phase.x);
        (&packed.x)[index * 2u + 1u] = static_cast<float>(phase.y);
    };
    storePhase(uniforms.foamOriginPhasesA, 0u,
               WorldPhase::periodicWorldPhase(m_activeCameraPosition, static_cast<double>(uniforms.foamDetailShape.z)));
    storePhase(uniforms.foamOriginPhasesA, 1u,
               WorldPhase::periodicWorldPhase(m_activeCameraPosition, static_cast<double>(uniforms.foamDetailBreakup.y)));
    storePhase(uniforms.foamOriginPhasesB, 0u,
               WorldPhase::periodicWorldPhase(
                   m_activeCameraPosition, static_cast<double>(1.0f / uniforms.foamDetailShape.x)));
    uniforms.foamEvolutionParams = glm::vec4(
        foamEvolutionStart,
        std::max(foamEvolutionEnd, foamEvolutionStart + 1.0e-4f),
        foamEvolutionDropoffEnd,
        0.0f);
    uniforms.foamFadeParams = glm::vec4(
        foamFadeStart,
        std::max(foamFadeEnd, foamFadeStart + 1.0e-4f),
        0.0f,
        0.0f);
    const float shoreFoamDepthStart = std::max(m_settings.shoreFoamDepthStart, 0.0f);
    const float shoreFoamDepthEnd = std::max(m_settings.shoreFoamDepthEnd, shoreFoamDepthStart + 1.0e-4f);
    uniforms.shorelineFoamParams = glm::vec4(
        (m_settings.drawFoam && m_settings.shoreFoamEnabled) ? std::max(m_settings.shoreFoamAmount, 0.0f) : 0.0f,
        shoreFoamDepthStart,
        shoreFoamDepthEnd,
        std::max(m_settings.shoreFoamBreakupStrength, 0.0f));
    const float shoreFoamDecayDepthStart = std::max(m_settings.shoreFoamDecayDepthStart, 0.0f);
    const float shoreFoamDecayDepthEnd = std::max(
        m_settings.shoreFoamDecayDepthEnd,
        shoreFoamDecayDepthStart + 1.0e-4f);
    uniforms.shorelineFoamDecayParams = glm::vec4(
        shoreFoamDecayDepthStart,
        shoreFoamDecayDepthEnd,
        0.0f,
        0.0f);
    uniforms.shallowWaterColor = glm::vec4(m_settings.shallowWaterColor, 0.0f);
    uniforms.midWaterColor = glm::vec4(m_settings.midWaterColor, 0.0f);
    uniforms.deepWaterColor = glm::vec4(m_settings.deepWaterColor, 0.0f);
    uniforms.waterDepthColorParams = glm::vec4(
        std::max(m_settings.midWaterDepthStart, 0.0f),
        std::max(m_settings.midWaterDepthEnd, m_settings.midWaterDepthStart + 1.0e-4f),
        std::max(m_settings.deepWaterDepthStart, 0.0f),
        std::max(m_settings.deepWaterDepthEnd, m_settings.deepWaterDepthStart + 1.0e-4f));
    uniforms.debugParams.y = AppConfig::Water::kSubsurfaceStrength;
    uniforms.debugParams.z = AppConfig::Water::kScatteringAnisotropy;
    uniforms.debugParams.w = AppConfig::Water::kDepthAbsorptionStrength;

    for (std::uint32_t cascadeIndex = 0; cascadeIndex < std::min(m_settings.cascadeCount, AppConfig::Water::kMaxCascadeCount); ++cascadeIndex)
    {
        const float worldSize = std::max(m_settings.cascades[cascadeIndex].worldSizeMeters, 1.0f);
        const float shallowDamping = std::max(m_settings.cascades[cascadeIndex].shallowDampingStrength, 0.0f);
        const float shallowDepthMeters = std::max(
            m_settings.cascades[cascadeIndex].shallowDampingDepthMeters,
            0.0f);
        if (cascadeIndex < 4u)
        {
            (&uniforms.cascadeWorldSizesA.x)[cascadeIndex] = worldSize;
            (&uniforms.cascadeShallowDampingA.x)[cascadeIndex] = shallowDamping;
            (&uniforms.cascadeShallowDepthA.x)[cascadeIndex] = shallowDepthMeters;
        }
        else
        {
            (&uniforms.cascadeWorldSizesB.x)[cascadeIndex - 4u] = worldSize;
            (&uniforms.cascadeShallowDampingB.x)[cascadeIndex - 4u] = shallowDamping;
            (&uniforms.cascadeShallowDepthB.x)[cascadeIndex - 4u] = shallowDepthMeters;
        }
        storePhase(cascadeIndex < 2u ? uniforms.cascadeOriginPhasesA : uniforms.cascadeOriginPhasesB,
                   cascadeIndex % 2u, WorldPhase::periodicWorldPhase(
                       m_activeCameraPosition, static_cast<double>(1.0f / worldSize)));
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

void QuadtreeWaterMeshRenderer::fillMediumUniforms(
    SkyboxRenderer::FragmentUniforms& uniforms, float viewportHeight) const
{
    uniforms.waterParams = glm::vec4(m_settings.waterLevel,
        static_cast<float>(m_settings.cascadeCount), m_settings.enabled ? 1.0f : 0.0f, 0.0f);
    uniforms.waterFilter = glm::vec4(viewportHeight,
        std::tan(AppConfig::Camera::kVerticalFovRadians * 0.5f),
        AppConfig::Water::kCascadeDetailTexelFadeStart, AppConfig::Water::kCascadeDetailTexelFadeEnd);
    uniforms.waterCameraLeaf = glm::vec4(0.0f, 0.0f, 1.0f, -1.0f);
    uniforms.waterDepthParams = glm::vec4(AppConfig::Water::kShallowDepthFadeStartMeters,
        AppConfig::Water::kShallowDepthFadeEndMeters, 15.0f, 0.0f);
    // Forward existing emitted terrain metadata only; water height stays entirely GPU-side.
    for (std::uint32_t i = 0; i < m_instanceCount; ++i)
    {
        const auto& instance = m_descriptors.parents[i].body;
        const float size = instance.leafParams.x;
        if (instance.position[0] <= 0.0f && instance.position[0] + size > 0.0f &&
            instance.position[2] <= 0.0f && instance.position[2] + size > 0.0f)
        {
            uniforms.waterDepthParams.z = static_cast<float>((instance.packedMetadata >> 16u) & 0xFFFFu);
            uniforms.waterCameraLeaf = glm::vec4(instance.position[0], instance.position[2],
                size, instance.leafParams.w > 0.5f ? instance.leafParams.z : -1.0f);
            break;
        }
    }
    for (std::uint32_t i = 0; i < std::min(m_settings.cascadeCount, AppConfig::Water::kMaxCascadeCount); ++i)
    {
        const auto& cascade = m_settings.cascades[i];
        const float size = std::max(cascade.worldSizeMeters, 1.0f);
        uniforms.waterSizes[i] = size;
        uniforms.waterDamping[i] = cascade.shallowDampingStrength;
        uniforms.waterShallowDepth[i] = cascade.shallowDampingDepthMeters;
        const auto phase = WorldPhase::periodicWorldPhase(m_activeCameraPosition, static_cast<double>(1.0f / size));
        auto& packed = i < 2u ? uniforms.waterPhasesA : uniforms.waterPhasesB;
        packed[(i % 2u)*2u] = static_cast<float>(phase.x);
        packed[(i % 2u)*2u+1u] = static_cast<float>(phase.y);
    }
}
