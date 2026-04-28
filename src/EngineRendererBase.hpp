#pragma once

#include "Position.hpp"

#include <SDL3/SDL_gpu.h>
#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <vector>

class EngineRendererBase
{
public:
    EngineRendererBase() = default;
    ~EngineRendererBase() = default;

    EngineRendererBase(const EngineRendererBase&) = delete;
    EngineRendererBase& operator=(const EngineRendererBase&) = delete;

protected:
    void initializeRendererBase(
        SDL_GPUDevice* device,
        SDL_GPUTextureFormat colorFormat,
        SDL_GPUTextureFormat depthFormat
    );

    [[nodiscard]] SDL_GPUShader* createShader(
        const std::filesystem::path& path,
        SDL_GPUShaderStage stage,
        std::uint32_t uniformBufferCount,
        std::uint32_t storageBufferCount = 0,
        std::uint32_t samplerCount = 0
    ) const;

    [[nodiscard]] std::vector<std::uint8_t> readShaderCode(const std::filesystem::path& path) const;
    [[noreturn]] void throwSdlError(const char* message) const;
    void setActiveCameraPosition(const Position& cameraPosition);
    [[nodiscard]] glm::vec3 localPositionFromWorldPosition(const Position& position) const;

    SDL_GPUDevice* m_device = nullptr;
    SDL_GPUTextureFormat m_colorFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTextureFormat m_depthFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    Position m_activeCameraPosition{};
};
