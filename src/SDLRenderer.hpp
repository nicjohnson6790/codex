#pragma once

#include "RenderTypes.hpp"
#include "Position.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <imgui.h>

#include <glm/mat4x4.hpp>

#include <string>

class LightingSystem;
class LineRenderer;
class QuadtreeMeshRenderer;
class TriangleRenderer;

class SDLRenderer
{
public:
    SDLRenderer() = default;
    ~SDLRenderer() = default;

    SDLRenderer(const SDLRenderer&) = delete;
    SDLRenderer& operator=(const SDLRenderer&) = delete;

    void initialize(SDL_Window* window);
    void shutdown();
    void waitIdle() const;

    void setActiveCamera(
        const Position& cameraPosition,
        TriangleRenderer& triangleRenderer,
        QuadtreeMeshRenderer& quadtreeMeshRenderer,
        LineRenderer& lineRenderer);
    void setViewportSize(Extent2D extent);
    void renderFrame(
        TriangleRenderer& triangleRenderer,
        QuadtreeMeshRenderer& quadtreeMeshRenderer,
        LineRenderer& lineRenderer,
        const glm::mat4& viewProjection,
        const LightingSystem& lightingSystem,
        ImDrawData* drawData,
        bool renderViewport
    );

    [[nodiscard]] ImTextureID viewportTextureId() const;
    [[nodiscard]] SDL_GPUDevice* device() const { return m_device; }
    [[nodiscard]] SDL_GPUTextureFormat swapchainFormat() const { return m_swapchainFormat; }
    [[nodiscard]] SDL_GPUTextureFormat viewportDepthFormat() const { return m_viewportDepthFormat; }
    [[nodiscard]] const std::string& driverName() const { return m_driverName; }

private:
    void createViewportTargets();
    void destroyViewportTargets();
    void recreateViewportTargetsIfNeeded();
    [[nodiscard]] SDL_GPUTextureFormat chooseViewportDepthFormat() const;

    SDL_Window* m_window = nullptr;
    SDL_GPUDevice* m_device = nullptr;
    std::string m_driverName;
    Position m_activeCameraPosition{};

    Extent2D m_requestedViewportExtent{ 640, 480 };
    Extent2D m_viewportExtent{ 0, 0 };

    SDL_GPUTextureFormat m_swapchainFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTextureFormat m_viewportDepthFormat = SDL_GPU_TEXTUREFORMAT_INVALID;

    SDL_GPUTexture* m_viewportColorTexture = nullptr;
    SDL_GPUTexture* m_viewportDepthTexture = nullptr;
};
