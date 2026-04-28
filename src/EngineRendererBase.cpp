#include "EngineRendererBase.hpp"

#include <SDL3/SDL.h>

#include <fstream>
#include <stdexcept>
#include <string>

void EngineRendererBase::initializeRendererBase(
    SDL_GPUDevice* device,
    SDL_GPUTextureFormat colorFormat,
    SDL_GPUTextureFormat depthFormat
)
{
    m_device = device;
    m_colorFormat = colorFormat;
    m_depthFormat = depthFormat;
}

void EngineRendererBase::setActiveCameraPosition(const Position& cameraPosition)
{
    m_activeCameraPosition = cameraPosition;
}

glm::vec3 EngineRendererBase::localPositionFromWorldPosition(const Position& position) const
{
    const glm::dvec3 relative = position.localCoordinatesInCellOf(m_activeCameraPosition);
    return {
        static_cast<float>(relative.x),
        static_cast<float>(relative.y),
        static_cast<float>(relative.z),
    };
}

SDL_GPUShader* EngineRendererBase::createShader(
    const std::filesystem::path& path,
    SDL_GPUShaderStage stage,
    std::uint32_t uniformBufferCount,
    std::uint32_t storageBufferCount,
    std::uint32_t samplerCount
) const
{
    const std::vector<std::uint8_t> bytes = readShaderCode(path);

    SDL_GPUShaderCreateInfo shaderInfo{};
    shaderInfo.code_size = bytes.size();
    shaderInfo.code = bytes.data();
    shaderInfo.entrypoint = "main";
    shaderInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    shaderInfo.stage = stage;
    shaderInfo.num_samplers = samplerCount;
    shaderInfo.num_storage_buffers = storageBufferCount;
    shaderInfo.num_uniform_buffers = uniformBufferCount;

    SDL_GPUShader* shader = SDL_CreateGPUShader(m_device, &shaderInfo);
    if (shader == nullptr)
    {
        throwSdlError(("Failed to create shader: " + path.string()).c_str());
    }
    return shader;
}

std::vector<std::uint8_t> EngineRendererBase::readShaderCode(const std::filesystem::path& path) const
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        throw std::runtime_error("Failed to open shader: " + path.string());
    }

    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

[[noreturn]] void EngineRendererBase::throwSdlError(const char* message) const
{
    throw std::runtime_error(std::string(message) + " " + SDL_GetError());
}
