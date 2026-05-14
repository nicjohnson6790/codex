#pragma once

#include "AppPanels.hpp"
#include "CameraManager.hpp"
#include "FoliageCanopyRenderer.hpp"
#include "FoliageImposterRenderer.hpp"
#include "FreeFlightCameraController.hpp"
#include "GamepadInput.hpp"
#include "Gameplay.hpp"
#include "LineRenderer.hpp"
#include "LightingSystem.hpp"
#include "NearbyFoliageRenderer.hpp"
#include "PerformanceCapture.hpp"
#include "platform/SteamService.hpp"
#include "QuadtreeMeshRenderer.hpp"
#include "QuadtreeWaterMeshRenderer.hpp"
#include "RenderEngines.hpp"
#include "SDLRenderer.hpp"
#include "SceneTypes.hpp"
#include "SkyboxRenderer.hpp"
#include "TriangleRenderer.hpp"
#include "WorldGridQuadtree.hpp"
#include "WorldGridFoliageCanopyManager.hpp"
#include "WorldGridFoliageManager.hpp"
#include "WorldGridQuadtreeWaterManager.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

class App
{
public:
    struct Options
    {
        bool verboseStartupLogging = false;
        bool quitAfterFirstFrame = false;
        bool enableSteam = true;
    };

    explicit App(const Options& options = {});

    void run();

private:
    void logStartup(std::string_view message) const;
    void initialize();
    void shutdown();

    void initializeWindowing();
    void initializePlatformServices();
    void initializeGameplayState();
    void initializeRenderers(const std::filesystem::path& shaderDirectory);
    void initializeImGui();

    void shutdownImGui();
    void shutdownRenderers();
    void shutdownPlatformServices();
    void shutdownWindowing();

    void updateFrameTiming();
    void pollEvents();
    void tickPlatformServices();
    GamepadState updateInputState();
    void updateFreeCamera(const GamepadState& gamepadState);
    void updateSimulationTime();
    void beginImGuiFrame();
    void buildUi();
    void syncCameraModeTransition();
    void updateSceneForFrame();
    void renderCurrentFrame();
    void finishFrame();

    void syncRenderStateForActiveCamera(Extent2D viewportExtent);
    void syncWaterStateForFrame();
    void collectFoliageReadbacks();
    void updateGameplayForFrame();
    void syncFollowCameraRenderState(Extent2D viewportExtent);
    void buildPrimitiveDraws();
    void buildDebugAxes();
    void emitWorldDraws();
    void scheduleGenerationWork();

    [[nodiscard]] std::vector<std::string> querySdlGpuDrivers() const;

    SDL_Window* m_window = nullptr;
    bool m_running = true;

    SDLRenderer m_renderer;
    TriangleRenderer m_triangleRenderer;
    LineRenderer m_lineRenderer;
    QuadtreeMeshRenderer m_quadtreeMeshRenderer;
    FoliageCanopyRenderer m_foliageCanopyRenderer;
    FoliageImposterRenderer m_foliageRenderer;
    NearbyFoliageRenderer m_nearbyFoliageRenderer;
    QuadtreeWaterMeshRenderer m_waterMeshRenderer;
    SkyboxRenderer m_skyboxRenderer;
    GamepadInput m_gamepadInput;
    SteamService m_steamService;
    CameraManager m_cameraManager;
    FreeFlightCameraController m_cameraController;
    PlayerPawn m_playerPawn;
    PlayerController m_playerController;
    CharacterMotor m_characterMotor;
    FollowCameraController m_followCameraController;
    CollisionManager m_collisionManager;
    PlayerMoveIntent m_playerMoveIntent;
    AppPanels m_panels;
    WorldGridQuadtree m_worldGridQuadtree;
    WorldGridFoliageCanopyManager m_foliageCanopyManager;
    WorldGridFoliageManager m_foliageManager;
    WorldGridQuadtreeWaterManager m_waterManager;
    LightingSystem m_lightingSystem;

    std::vector<TriangleInstance> m_instances{
        { .position = Position(-1, 0, { static_cast<double>(Position::kCellSize) - 0.25, 0.0, 0.0 }) },
        { .position = Position(0, 0, { 0.25, 0.0, 0.0 }) },
    };

    std::vector<std::string> m_gpuDrivers;
    Options m_options{};
    std::size_t m_freeCameraIndex = 0;
    std::size_t m_playerCameraIndex = 0;
    bool m_playerFollowCameraEnabled = false;
    bool m_previousPlayerFollowCameraEnabled = false;
    bool m_firstFramePresented = false;
    std::uint64_t m_lastFrameTsc = 0;
    std::uint64_t m_frameIndex = 0;
    float m_deltaTimeSeconds = 1.0f / 60.0f;
    float m_elapsedTimeSeconds = 0.0f;
};
