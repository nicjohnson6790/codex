#pragma once

#include <SDL3/SDL_gpu.h>

struct SubmittedGpuFence
{
    SDL_GPUDevice* device = nullptr;
    SDL_GPUFence* fence = nullptr;

    SubmittedGpuFence() = default;
    SubmittedGpuFence(SDL_GPUDevice* inDevice, SDL_GPUFence* inFence)
        : device(inDevice)
        , fence(inFence)
    {
    }

    SubmittedGpuFence(const SubmittedGpuFence&) = delete;
    SubmittedGpuFence& operator=(const SubmittedGpuFence&) = delete;

    ~SubmittedGpuFence()
    {
        if (device != nullptr && fence != nullptr)
        {
            SDL_ReleaseGPUFence(device, fence);
        }
    }

    [[nodiscard]] bool isSignaled() const
    {
        return device != nullptr && fence != nullptr && SDL_QueryGPUFence(device, fence);
    }
};
