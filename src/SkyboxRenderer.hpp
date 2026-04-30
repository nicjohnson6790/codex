#pragma once

#include "AppConfig.hpp"
#include "EngineRendererBase.hpp"
#include "LightingSystem.hpp"

#include <SDL3/SDL_gpu.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <filesystem>

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
        float exposure = AppConfig::Atmosphere::kExposure;
        float alphaScale = AppConfig::Atmosphere::kAlphaScale;
        float ambientSkyScale = AppConfig::Atmosphere::kAmbientSkyScale;
        float ambientBlueBias = AppConfig::Atmosphere::kAmbientBlueBias;
        float ambientSolarInfluence = AppConfig::Atmosphere::kAmbientSolarInfluence;
        float ambientTwilightInfluence = AppConfig::Atmosphere::kAmbientTwilightInfluence;
        float ambientBlueTintR = AppConfig::Atmosphere::kAmbientBlueTint.r;
        float ambientBlueTintG = AppConfig::Atmosphere::kAmbientBlueTint.g;
        float ambientBlueTintB = AppConfig::Atmosphere::kAmbientBlueTint.b;
        float rayleighTintScale = AppConfig::Atmosphere::kRayleighTintScale;
        float hazeColorR = AppConfig::Atmosphere::kHazeColor.r;
        float hazeColorG = AppConfig::Atmosphere::kHazeColor.g;
        float hazeColorB = AppConfig::Atmosphere::kHazeColor.b;
        float hazeStrength = AppConfig::Atmosphere::kHazeStrength;
        float pathFogDistance = AppConfig::Atmosphere::kPathFogDistance;
        float longRangeHazeDistance = AppConfig::Atmosphere::kLongRangeHazeDistance;
        float aureolePower = AppConfig::Atmosphere::kAureolePower;
        float aureoleStrength = AppConfig::Atmosphere::kAureoleStrength;
        float sunDiskPower = AppConfig::Atmosphere::kSunDiskPower;
        float sunDiskStrength = AppConfig::Atmosphere::kSunDiskStrength;
        float sunGlowPower = AppConfig::Atmosphere::kSunGlowPower;
        float sunsetTintR = AppConfig::Atmosphere::kSunsetTint.r;
        float sunsetTintG = AppConfig::Atmosphere::kSunsetTint.g;
        float sunsetTintB = AppConfig::Atmosphere::kSunsetTint.b;
        float sunsetStrength = AppConfig::Atmosphere::kSunsetStrength;
        float sunsetSunwardBoost = AppConfig::Atmosphere::kSunsetSunwardBoost;
        float sunsetDistanceMin = AppConfig::Atmosphere::kSunsetDistanceMin;
        float sunsetDistanceMax = AppConfig::Atmosphere::kSunsetDistanceMax;
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
    };

    struct SharedSkyUniforms
    {
        glm::mat4 skyRotation{1.0f};
        glm::vec4 atmosphereParams{0.0f};
        glm::vec4 sunDirectionTimeOfDay{0.0f};
    };

    static constexpr std::uint32_t kAtmosphereLutResolution = 32;

    void initialize(
        SDL_GPUDevice* device,
        SDL_GPUTextureFormat colorFormat,
        SDL_GPUTextureFormat depthFormat,
        const std::filesystem::path& shaderDirectory,
        const std::filesystem::path& resourceDirectory
    );
    void shutdown();

    void render(
        SDL_GPURenderPass* renderPass,
        SDL_GPUCommandBuffer* commandBuffer,
        const glm::mat4& inverseViewProjection,
        SDL_GPUTexture* depthTexture,
        float cameraAltitude,
        const LightingSystem& lightingSystem) const;
    [[nodiscard]] SharedSkyUniforms buildSharedSkyUniforms(
        float cameraAltitude,
        const LightingSystem& lightingSystem) const;
    [[nodiscard]] AtmosphereSettings& atmosphereSettings() { return m_atmosphereSettings; }
    [[nodiscard]] const AtmosphereSettings& atmosphereSettings() const { return m_atmosphereSettings; }
    [[nodiscard]] SDL_GPUTexture* cubemapTexture() const { return m_cubemapTexture; }
    [[nodiscard]] SDL_GPUTexture* atmosphereLutTexture() const { return m_atmosphereLutTexture; }
    [[nodiscard]] SDL_GPUSampler* cubemapSampler() const { return m_cubemapSampler; }
    [[nodiscard]] SDL_GPUSampler* atmosphereSampler() const { return m_atmosphereSampler; }
    void regenerateAtmosphereLut();
    void resetAtmosphereSettings();
    void sanitizeAtmosphereSettings();

private:
    void createPipeline(const std::filesystem::path& shaderDirectory);
    void createStaticVertexResources();
    void createCubemapTexture(const std::filesystem::path& resourceDirectory);
    void createAtmosphereLutTexture();
    [[nodiscard]] std::array<std::uint8_t, kAtmosphereLutResolution * kAtmosphereLutResolution * kAtmosphereLutResolution * 4> buildAtmosphereLut() const;

    SDL_GPUGraphicsPipeline* m_pipeline = nullptr;
    SDL_GPUBuffer* m_vertexBuffer = nullptr;
    SDL_GPUTransferBuffer* m_vertexTransferBuffer = nullptr;
    SDL_GPUTexture* m_cubemapTexture = nullptr;
    SDL_GPUTexture* m_atmosphereLutTexture = nullptr;
    SDL_GPUSampler* m_cubemapSampler = nullptr;
    SDL_GPUSampler* m_atmosphereSampler = nullptr;
    SDL_GPUSampler* m_depthSampler = nullptr;
    AtmosphereSettings m_atmosphereSettings{};
};
