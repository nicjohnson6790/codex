#include "SDLRenderer.hpp"

#include "AppConfig.hpp"
#include "LightingSystem.hpp"
#include "LineRenderer.hpp"
#include "PerformanceCapture.hpp"
#include "QuadtreeMeshRenderer.hpp"
#include "SkyboxRenderer.hpp"
#include "TriangleRenderer.hpp"

#include <imgui_impl_sdlgpu3.h>

#include <algorithm>
#include <array>
#include <glm/matrix.hpp>
#include <stdexcept>

namespace
{
void throwSdlError(const char* message)
{
    throw std::runtime_error(std::string(message) + " " + SDL_GetError());
}
}

void SDLRenderer::initialize(SDL_Window* window)
{
    m_window = window;

    SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0)
    {
        throwSdlError("Failed to create SDL GPU properties.");
    }

    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, true);
    SDL_SetStringProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_INDIRECT_DRAW_FIRST_INSTANCE_BOOLEAN, true);

    m_device = SDL_CreateGPUDeviceWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (m_device == nullptr)
    {
        throwSdlError("Failed to create SDL GPU device.");
    }

    if (!SDL_ClaimWindowForGPUDevice(m_device, m_window))
    {
        throwSdlError("Failed to claim SDL window for SDL GPU device.");
    }

    if (!SDL_SetGPUSwapchainParameters(m_device, m_window, AppConfig::Renderer::kSwapchainComposition, AppConfig::Renderer::kPresentMode))
    {
        throwSdlError("Failed to configure SDL GPU swapchain parameters.");
    }

    m_driverName = SDL_GetGPUDeviceDriver(m_device);
    m_swapchainFormat = SDL_GetGPUSwapchainTextureFormat(m_device, m_window);
    m_viewportDepthFormat = chooseViewportDepthFormat();
    createViewportTargets();
}

void SDLRenderer::shutdown()
{
    if (m_device == nullptr)
    {
        return;
    }

    waitIdle();
    destroyViewportTargets();

    if (m_window != nullptr)
    {
        SDL_ReleaseWindowFromGPUDevice(m_device, m_window);
    }

    SDL_DestroyGPUDevice(m_device);
    m_device = nullptr;
    m_window = nullptr;
    m_driverName.clear();
    m_swapchainFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    m_viewportDepthFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
}

void SDLRenderer::waitIdle() const
{
    if (m_device != nullptr && !SDL_WaitForGPUIdle(m_device))
    {
        throwSdlError("Failed while waiting for SDL GPU device idle.");
    }
}

void SDLRenderer::setActiveCamera(
    const Position& cameraPosition,
    TriangleRenderer& triangleRenderer,
    QuadtreeMeshRenderer& quadtreeMeshRenderer,
    LineRenderer& lineRenderer)
{
    m_activeCameraPosition = cameraPosition;
    triangleRenderer.setActiveCamera(cameraPosition);
    quadtreeMeshRenderer.setActiveCamera(cameraPosition);
    lineRenderer.setActiveCamera(cameraPosition);
}

void SDLRenderer::setViewportSize(Extent2D extent)
{
    m_requestedViewportExtent.width = std::max(1u, extent.width);
    m_requestedViewportExtent.height = std::max(1u, extent.height);
}

ImTextureID SDLRenderer::viewportTextureId() const
{
    return reinterpret_cast<ImTextureID>(m_viewportColorTexture);
}

void SDLRenderer::renderFrame(
    TriangleRenderer& triangleRenderer,
    QuadtreeMeshRenderer& quadtreeMeshRenderer,
    LineRenderer& lineRenderer,
    SkyboxRenderer& skyboxRenderer,
    const glm::mat4& viewProjection,
    const LightingSystem& lightingSystem,
    ImDrawData* drawData,
    bool renderViewport
)
{
    HELLO_PROFILE_SCOPE("SDLRenderer::RenderFrame");

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (commandBuffer == nullptr)
    {
        throwSdlError("Failed to acquire SDL GPU command buffer.");
    }

    {
        HELLO_PROFILE_SCOPE("SDLRenderer::UploadGeometry");
        SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
        if (copyPass == nullptr)
        {
            throwSdlError("Failed to begin SDL GPU copy pass.");
        }
        triangleRenderer.upload(copyPass);
        quadtreeMeshRenderer.upload(copyPass);
        lineRenderer.upload(copyPass);
        SDL_EndGPUCopyPass(copyPass);
    }

    quadtreeMeshRenderer.dispatchHeightmapGenerations(commandBuffer);

    {
        HELLO_PROFILE_SCOPE("SDLRenderer::DownloadTerrainExtents");
        SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
        if (copyPass == nullptr)
        {
            throwSdlError("Failed to begin SDL GPU extents download copy pass.");
        }
        quadtreeMeshRenderer.queueHeightmapExtentsDownload(copyPass);
        SDL_EndGPUCopyPass(copyPass);
    }

    if (drawData != nullptr)
    {
        HELLO_PROFILE_SCOPE_GROUPS(
            "SDLRenderer::PrepareImGui",
            ProfileScopeGroup::ImGui | ProfileScopeGroup::Renderer);
        ImGui_ImplSDLGPU3_PrepareDrawData(drawData, commandBuffer);
    }

    if (renderViewport)
    {
        HELLO_PROFILE_SCOPE("SDLRenderer::RenderViewport");

        SDL_GPUColorTargetInfo colorTargetInfo{};
        colorTargetInfo.texture = m_viewportColorTexture;
        colorTargetInfo.clear_color = SDL_FColor{ 0.08f, 0.09f, 0.12f, 1.0f };
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPUDepthStencilTargetInfo depthTargetInfo{};
        depthTargetInfo.texture = m_viewportDepthTexture;
        depthTargetInfo.clear_depth = 0.0f;
        depthTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        depthTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
        depthTargetInfo.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
        depthTargetInfo.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        depthTargetInfo.clear_stencil = 0;

        SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(commandBuffer, &colorTargetInfo, 1, &depthTargetInfo);
        if (renderPass == nullptr)
        {
            throwSdlError("Failed to begin SDL GPU viewport render pass.");
        }

        SDL_GPUViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.w = static_cast<float>(m_viewportExtent.width);
        viewport.h = static_cast<float>(m_viewportExtent.height);
        viewport.min_depth = 0.0f;
        viewport.max_depth = 1.0f;
        SDL_SetGPUViewport(renderPass, &viewport);

        const SDL_Rect scissor{
            0,
            0,
            static_cast<int>(m_viewportExtent.width),
            static_cast<int>(m_viewportExtent.height)
        };
        SDL_SetGPUScissor(renderPass, &scissor);

        quadtreeMeshRenderer.render(renderPass, commandBuffer, viewProjection, lightingSystem);
        triangleRenderer.render(renderPass, commandBuffer, viewProjection);
        lineRenderer.render(renderPass, commandBuffer, viewProjection);
        skyboxRenderer.render(renderPass, commandBuffer, glm::inverse(viewProjection), lightingSystem);
        SDL_EndGPURenderPass(renderPass);
    }

    SDL_GPUTexture* swapchainTexture = nullptr;
    Uint32 swapchainWidth = 0;
    Uint32 swapchainHeight = 0;
    {
        HELLO_PROFILE_SCOPE_GROUPS(
            "SDLRenderer::AcquireSwapchain",
            ProfileScopeGroup::Wait);
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, m_window, &swapchainTexture, &swapchainWidth, &swapchainHeight))
        {
            throwSdlError("Failed to acquire SDL GPU swapchain texture.");
        }
    }

    if (swapchainTexture != nullptr && drawData != nullptr)
    {
        HELLO_PROFILE_SCOPE_GROUPS(
            "SDLRenderer::RenderImGui",
            ProfileScopeGroup::ImGui | ProfileScopeGroup::Renderer);

        SDL_GPUColorTargetInfo colorTargetInfo{};
        colorTargetInfo.texture = swapchainTexture;
        colorTargetInfo.clear_color = SDL_FColor{ 0.11f, 0.11f, 0.12f, 1.0f };
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(commandBuffer, &colorTargetInfo, 1, nullptr);
        if (renderPass == nullptr)
        {
            throwSdlError("Failed to begin SDL GPU swapchain render pass.");
        }

        SDL_GPUViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.w = static_cast<float>(swapchainWidth);
        viewport.h = static_cast<float>(swapchainHeight);
        viewport.min_depth = 0.0f;
        viewport.max_depth = 1.0f;
        SDL_SetGPUViewport(renderPass, &viewport);

        ImGui_ImplSDLGPU3_RenderDrawData(drawData, commandBuffer, renderPass);
        SDL_EndGPURenderPass(renderPass);
    }

    SDL_GPUFence* submittedFence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
    quadtreeMeshRenderer.attachSubmittedFence(submittedFence);
    if (submittedFence == nullptr)
    {
        throwSdlError("Failed to submit SDL GPU command buffer.");
    }

    // ImGui draw data for this frame may still reference the previous viewport texture,
    // so defer resizing until after the command buffer using it has been submitted.
    recreateViewportTargetsIfNeeded();
}

void SDLRenderer::createViewportTargets()
{
    m_viewportExtent.width = std::max(1u, m_requestedViewportExtent.width);
    m_viewportExtent.height = std::max(1u, m_requestedViewportExtent.height);

    SDL_GPUTextureCreateInfo colorInfo{};
    colorInfo.type = SDL_GPU_TEXTURETYPE_2D;
    colorInfo.format = m_swapchainFormat;
    colorInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    colorInfo.width = m_viewportExtent.width;
    colorInfo.height = m_viewportExtent.height;
    colorInfo.layer_count_or_depth = 1;
    colorInfo.num_levels = 1;
    colorInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    m_viewportColorTexture = SDL_CreateGPUTexture(m_device, &colorInfo);
    if (m_viewportColorTexture == nullptr)
    {
        throwSdlError("Failed to create SDL GPU viewport color texture.");
    }

    SDL_GPUTextureCreateInfo depthInfo{};
    depthInfo.type = SDL_GPU_TEXTURETYPE_2D;
    depthInfo.format = m_viewportDepthFormat;
    depthInfo.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    depthInfo.width = m_viewportExtent.width;
    depthInfo.height = m_viewportExtent.height;
    depthInfo.layer_count_or_depth = 1;
    depthInfo.num_levels = 1;
    depthInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    m_viewportDepthTexture = SDL_CreateGPUTexture(m_device, &depthInfo);
    if (m_viewportDepthTexture == nullptr)
    {
        throwSdlError("Failed to create SDL GPU viewport depth texture.");
    }
}

void SDLRenderer::destroyViewportTargets()
{
    if (m_viewportDepthTexture != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_viewportDepthTexture);
        m_viewportDepthTexture = nullptr;
    }

    if (m_viewportColorTexture != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, m_viewportColorTexture);
        m_viewportColorTexture = nullptr;
    }

    m_viewportExtent = { 0, 0 };
}

void SDLRenderer::recreateViewportTargetsIfNeeded()
{
    if (m_requestedViewportExtent.width == m_viewportExtent.width &&
        m_requestedViewportExtent.height == m_viewportExtent.height)
    {
        return;
    }

    waitIdle();
    destroyViewportTargets();
    createViewportTargets();
}

SDL_GPUTextureFormat SDLRenderer::chooseViewportDepthFormat() const
{
    const std::array<SDL_GPUTextureFormat, 3> candidates{
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT,
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
        SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
    };

    for (SDL_GPUTextureFormat format : candidates)
    {
        if (SDL_GPUTextureSupportsFormat(
            m_device,
            format,
            SDL_GPU_TEXTURETYPE_2D,
            SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
        {
            return format;
        }
    }

    throw std::runtime_error("Failed to find a supported SDL GPU depth format.");
}
