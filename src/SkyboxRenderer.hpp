#pragma once

#include "AppConfig.hpp"
#include "EngineRendererBase.hpp"
#include "LightingSystem.hpp"
#include "assets/RuntimeAssetReader.hpp"

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <filesystem>

class QuadtreeWaterMeshRenderer;

class SkyboxRenderer : private EngineRendererBase
{
public:
    struct AtmosphereSettings
    {
        float atmosphereHeight = AppConfig::Atmosphere::kHeight;
        float atmosphereDistanceRange = AppConfig::Atmosphere::kDistanceRange;
        float rayleighScatterR = AppConfig::Atmosphere::kRayleighScatterR;
        float rayleighScatterG = AppConfig::Atmosphere::kRayleighScatterG;
        float rayleighScatterB = AppConfig::Atmosphere::kRayleighScatterB;
        float mieScatter = AppConfig::Atmosphere::kMieScatter;
        float mieExtinction = AppConfig::Atmosphere::kMieExtinction;
        float ozoneAbsorptionR = AppConfig::Atmosphere::kOzoneAbsorptionR;
        float ozoneAbsorptionG = AppConfig::Atmosphere::kOzoneAbsorptionG;
        float ozoneAbsorptionB = AppConfig::Atmosphere::kOzoneAbsorptionB;
        float rayleighScaleHeight = AppConfig::Atmosphere::kRayleighScaleHeight;
        float mieScaleHeight = AppConfig::Atmosphere::kMieScaleHeight;
        float ozoneColumnHeight = AppConfig::Atmosphere::kOzoneColumnHeight;
        float mieG = AppConfig::Atmosphere::kMieG;
        float atmosphereCloudSourceScale = AppConfig::Atmosphere::kAtmosphereCloudSourceScale;
    };

    struct AtmosphereOptics
    {
        glm::vec4 rayleigh{};
        glm::vec4 mie{};
        glm::vec4 ozone{};
        glm::vec4 solar{};
        glm::vec4 radianceScales{};
    };

    struct WaterMediumSettings
    {
        float sourceScale = AppConfig::Water::kMediumSourceScale;
        glm::vec3 absorption{AppConfig::Water::kMediumAbsorption};
        glm::vec3 scattering{AppConfig::Water::kMediumScattering};
    };

    struct Vertex
    {
        float position[2];
    };

    struct FragmentUniforms
    {
        glm::mat4 inverseViewProjection{1.0f};
        glm::mat4 skyRotation{1.0f};
        glm::vec4 atmosphereParams{0.0f};
        glm::vec4 sunDirectionTimeOfDay{0.0f};
        AtmosphereOptics optics{};
        glm::vec4 waterParams{}; // level, cascade count, enabled, draw mode
        glm::vec4 waterSizes{};
        glm::vec4 waterPhasesA{};
        glm::vec4 waterPhasesB{};
        glm::vec4 waterAbsorption{};
        glm::vec4 waterScattering{};
        glm::vec4 waterDamping{};
        glm::vec4 waterShallowDepth{};
        glm::vec4 waterCameraLeaf{}; // relative XZ origin, size, terrain slice (-1 if absent)
        glm::vec4 waterFilter{}; // viewport height, tan half FOV, detail fade start/end
        glm::vec4 waterDepthParams{}; // default local depth, shallow fade end, band mask
    };

    static_assert(sizeof(AtmosphereOptics) == 80);
    static_assert(offsetof(FragmentUniforms, optics) == 160);
    static_assert(sizeof(FragmentUniforms) == 416);

    struct SharedSkyUniforms
    {
        glm::mat4 skyRotation{1.0f};
        glm::vec4 atmosphereParams{0.0f};
        glm::vec4 sunDirectionTimeOfDay{0.0f};
    };


    void initialize(
        SDL_GPUDevice* device,
        SDL_GPUTextureFormat colorFormat,
        SDL_GPUTextureFormat depthFormat,
        const std::filesystem::path& shaderDirectory
    );
    void shutdown();

    void render(
        SDL_GPURenderPass* renderPass,
        SDL_GPUCommandBuffer* commandBuffer,
        const glm::mat4& inverseViewProjection,
        SDL_GPUTexture* depthTexture,
        float cameraAltitude,
        const LightingSystem& lightingSystem,
        const QuadtreeWaterMeshRenderer& waterRenderer,
        SDL_GPUBuffer* terrainHeightmapBuffer,
        float viewportHeight) const;
    [[nodiscard]] SharedSkyUniforms buildSharedSkyUniforms(
        float cameraAltitude,
        const LightingSystem& lightingSystem) const;
    [[nodiscard]] AtmosphereSettings& atmosphereSettings() { return m_atmosphereSettings; }
    [[nodiscard]] const AtmosphereSettings& atmosphereSettings() const { return m_atmosphereSettings; }
    [[nodiscard]] SDL_GPUTexture* cubemapTexture() const { return m_cubemapTexture; }
    [[nodiscard]] SDL_GPUSampler* cubemapSampler() const { return m_cubemapSampler; }
    [[nodiscard]] AtmosphereOptics buildAtmosphereOptics(const LightingSystem& lighting) const;
    [[nodiscard]] WaterMediumSettings& waterMediumSettings() { return m_waterMediumSettings; }
    [[nodiscard]] const WaterMediumSettings& waterMediumSettings() const { return m_waterMediumSettings; }
    void resetAtmosphereSettings();
    void sanitizeAtmosphereSettings();

private:
    void createPipeline(const std::filesystem::path& shaderDirectory);
    void createStaticVertexResources();
    void createCubemapTexture();

    std::array<SDL_GPUGraphicsPipeline*, 3> m_pipelines{};
    SDL_GPUBuffer* m_vertexBuffer = nullptr;
    SDL_GPUTransferBuffer* m_vertexTransferBuffer = nullptr;
    SDL_GPUTexture* m_cubemapTexture = nullptr;
    SDL_GPUSampler* m_cubemapSampler = nullptr;
    SDL_GPUSampler* m_depthSampler = nullptr;
    AtmosphereSettings m_atmosphereSettings{};
    WaterMediumSettings m_waterMediumSettings{};
};
