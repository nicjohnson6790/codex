#include "App.hpp"

#include "AppConfig.hpp"
#include "FoliageTypes.hpp"
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_gpu.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <glm/matrix.hpp>
#include <glm/vec3.hpp>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace
{
std::filesystem::path executableRelativePath(const std::filesystem::path& relativePath)
{
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr)
    {
        throw std::runtime_error(std::string("Failed to resolve executable base path: ") + SDL_GetError());
    }

    return std::filesystem::path(basePath) / relativePath;
}
}

App::App(const Options& options)
    : m_options(options)
{
}

void App::logStartup(std::string_view message) const
{
    {
        std::ofstream logFile("launch.log", std::ios::app);
        if (logFile)
        {
            logFile << "[startup] " << message << '\n';
        }
    }

    if (!m_options.verboseStartupLogging)
    {
        return;
    }

    std::cout << "[startup] " << message << '\n';
}

void App::run()
{
    logStartup("run begin");
    initialize();

    PerformanceCapture& performanceCapture = PerformanceCapture::instance();

    while (m_running)
    {
        const std::uint64_t frameStartTsc = PerformanceCapture::readTimestamp();
        const float deltaTimeSeconds = m_lastFrameTsc == 0
            ? (1.0f / 60.0f)
            : (performanceCapture.cyclesToMilliseconds(frameStartTsc - m_lastFrameTsc) / 1000.0f);
        m_deltaTimeSeconds = deltaTimeSeconds;
        m_lastFrameTsc = frameStartTsc;

        performanceCapture.setPaused(m_panels.viewportPaused());
        performanceCapture.beginFrame();

        pollEvents();

        const GamepadState gamepadState = m_gamepadInput.pollState();
        m_playerMoveIntent = m_playerController.poll(gamepadState);
        if (!m_playerFollowCameraEnabled &&
            m_cameraManager.hasActiveCamera() &&
            m_cameraManager.activeCameraIndex() == m_playerCameraIndex)
        {
            m_cameraManager.setActiveCamera(m_freeCameraIndex);
        }

        {
            HELLO_PROFILE_SCOPE("App::UpdateCamera");
            if (!m_panels.viewportPaused() && m_cameraManager.hasActiveCamera() && !m_playerFollowCameraEnabled)
            {
                m_cameraController.update(m_cameraManager.activeCamera(), gamepadState, deltaTimeSeconds);
            }
        }

        if (!m_panels.viewportPaused())
        {
            m_lightingSystem.advanceTime(deltaTimeSeconds);
        }
        m_elapsedTimeSeconds += deltaTimeSeconds;

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        {
            HELLO_PROFILE_SCOPE("App::BuildUi");
            buildUi();
            syncCameraModeTransition();
        }

        {
            HELLO_PROFILE_SCOPE("App::UpdateScene");
            updateSceneForFrame();
        }

        ImGui::Render();
        const glm::mat4 viewProjection = m_cameraManager.buildActiveViewProjectionMatrix(m_panels.viewportExtent());
        m_renderer.renderFrame(
            m_triangleRenderer,
            m_quadtreeMeshRenderer,
            m_foliageCanopyRenderer,
            m_foliageRenderer,
            m_nearbyFoliageRenderer,
            m_waterMeshRenderer,
            m_lineRenderer,
            m_skyboxRenderer,
            viewProjection,
            m_lightingSystem,
            m_panels.viewportExtent(),
            ImGui::GetDrawData(),
            !m_panels.viewportPaused(),
            m_elapsedTimeSeconds,
            m_frameIndex
        );
        ++m_frameIndex;

        performanceCapture.endFrame();

        if (!m_firstFramePresented)
        {
            m_firstFramePresented = true;
            logStartup("first frame submitted");
            if (m_options.quitAfterFirstFrame)
            {
                logStartup("quit-after-first-frame requested");
                m_running = false;
            }
        }
    }

    logStartup("shutdown begin");
    shutdown();
    logStartup("shutdown complete");
}

void App::initialize()
{
    logStartup("SDL init");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        throw std::runtime_error(SDL_GetError());
    }

    logStartup("create window");
    m_window = SDL_CreateWindow(
        "Terrain Sandbox",
        1780,
        900,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );
    if (m_window == nullptr)
    {
        throw std::runtime_error(SDL_GetError());
    }

    logStartup("window created");
    logStartup("query SDL GPU drivers");
    m_gpuDrivers = querySdlGpuDrivers();
    logStartup("queried SDL GPU drivers");
    logStartup("init gamepad input");
    m_gamepadInput.initialize();
    logStartup("gamepad input initialized");
    logStartup("init performance capture");
    PerformanceCapture::instance().initialize(AppConfig::Perf::kHistorySeconds);
    logStartup("performance capture initialized");
    logStartup("create default camera");
    m_freeCameraIndex = m_cameraManager.createCamera(
        "Camera 1",
        Position(0, 0, { 0.0, 300.0, 2.8 })
    );
    m_playerCameraIndex = m_cameraManager.createCamera(
        "Player Follow",
        Position(0, 0, { 0.0, 308.0, 8.0 }),
        { 0.0, -0.35, -1.0 },
        AppConfig::Camera::kWorldUp
    );
    m_cameraManager.setActiveCamera(m_freeCameraIndex);
    logStartup("default camera created");

    logStartup("init SDL GPU renderer");
    m_renderer.initialize(m_window);
    logStartup("SDL GPU renderer initialized");

    const std::filesystem::path shaderDirectory = executableRelativePath(TERRAIN_SANDBOX_SHADER_DIR);
    logStartup("init triangle renderer");
    m_triangleRenderer.initialize(
        m_renderer.device(),
        m_renderer.swapchainFormat(),
        m_renderer.viewportDepthFormat(),
        shaderDirectory
    );
    logStartup("init line renderer");
    m_lineRenderer.initialize(
        m_renderer.device(),
        m_renderer.swapchainFormat(),
        m_renderer.viewportDepthFormat(),
        shaderDirectory
    );
    logStartup("init quadtree mesh renderer");
    m_quadtreeMeshRenderer.initialize(
        m_renderer.device(),
        m_renderer.swapchainFormat(),
        m_renderer.viewportDepthFormat(),
        shaderDirectory
    );
    if constexpr (AppConfig::Foliage::kCanopyEnabled)
    {
        logStartup("init foliage canopy renderer");
        m_foliageCanopyRenderer.initialize(
            m_renderer.device(),
            m_renderer.swapchainFormat(),
            m_renderer.viewportDepthFormat(),
            shaderDirectory
        );
    }
    if constexpr (AppConfig::Foliage::kEnabled)
    {
        logStartup("init foliage renderer");
        m_foliageRenderer.initialize(
            m_renderer.device(),
            m_renderer.swapchainFormat(),
            m_renderer.viewportDepthFormat(),
            shaderDirectory
        );
        logStartup("init nearby foliage renderer");
        m_nearbyFoliageRenderer.initialize(
            m_renderer.device(),
            m_renderer.swapchainFormat(),
            m_renderer.viewportDepthFormat(),
            shaderDirectory
        );
    }
    if constexpr (AppConfig::Water::kEnabled)
    {
        logStartup("init water mesh renderer");
        m_waterMeshRenderer.initialize(
            m_renderer.device(),
            m_renderer.swapchainFormat(),
            m_renderer.viewportDepthFormat(),
            shaderDirectory
        );
    }
    logStartup("init skybox renderer");
    m_skyboxRenderer.initialize(
        m_renderer.device(),
        m_renderer.swapchainFormat(),
        m_renderer.viewportDepthFormat(),
        shaderDirectory
    );

    logStartup("create ImGui context");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();

    logStartup("init ImGui SDL backend");
    ImGui_ImplSDL3_InitForSDLGPU(m_window);

    ImGui_ImplSDLGPU3_InitInfo initInfo{};
    initInfo.Device = m_renderer.device();
    initInfo.ColorTargetFormat = m_renderer.swapchainFormat();
    initInfo.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    initInfo.SwapchainComposition = AppConfig::Renderer::kSwapchainComposition;
    initInfo.PresentMode = AppConfig::Renderer::kPresentMode;

    logStartup("init ImGui SDL GPU backend");
    ImGui_ImplSDLGPU3_Init(&initInfo);
    m_lastFrameTsc = PerformanceCapture::readTimestamp();
    logStartup("initialize complete");
}

void App::shutdown()
{
    logStartup("wait for device idle");
    m_renderer.waitIdle();

    logStartup("shutdown ImGui");
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    logStartup("shutdown app subsystems");
    PerformanceCapture::instance().shutdown();
    m_gamepadInput.shutdown();
    m_quadtreeMeshRenderer.shutdown();
    if constexpr (AppConfig::Foliage::kCanopyEnabled)
    {
        m_foliageCanopyRenderer.shutdown();
    }
    if constexpr (AppConfig::Foliage::kEnabled)
    {
        m_foliageRenderer.shutdown();
        m_nearbyFoliageRenderer.shutdown();
    }
    if constexpr (AppConfig::Water::kEnabled)
    {
        m_waterMeshRenderer.shutdown();
    }
    m_skyboxRenderer.shutdown();
    m_lineRenderer.shutdown();
    m_triangleRenderer.shutdown();
    m_renderer.shutdown();

    if (m_window != nullptr)
    {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }

    SDL_Quit();
}

void App::pollEvents()
{
    HELLO_PROFILE_SCOPE("App::PollEvents");

    SDL_Event event{};
    while (SDL_PollEvent(&event))
    {
        ImGui_ImplSDL3_ProcessEvent(&event);
        m_gamepadInput.handleEvent(event);

        if (event.type == SDL_EVENT_QUIT)
        {
            m_running = false;
        }
    }
}

void App::buildUi()
{
    AppPanels::Context context{
        .cameraManager = m_cameraManager,
        .renderer = m_renderer,
        .playerPawn = m_playerPawn,
        .collisionManager = m_collisionManager,
        .playerFollowCameraEnabled = m_playerFollowCameraEnabled,
        .gpuDrivers = m_gpuDrivers,
        .gamepadName = m_gamepadInput.gamepadName(),
        .lightingSystem = m_lightingSystem,
        .skyboxRenderer = m_skyboxRenderer,
        .foliageCanopyRenderer = m_foliageCanopyRenderer,
        .foliageCanopyManager = m_foliageCanopyManager,
        .foliageRenderer = m_foliageRenderer,
        .nearbyFoliageRenderer = m_nearbyFoliageRenderer,
        .foliageManager = m_foliageManager,
        .waterMeshRenderer = m_waterMeshRenderer,
        .waterManager = m_waterManager,
        .worldGridQuadtree = m_worldGridQuadtree,
        .viewportTextureId = m_renderer.viewportTextureId(),
    };
    m_panels.draw(context);
}

void App::syncCameraModeTransition()
{
    if (m_playerFollowCameraEnabled == m_previousPlayerFollowCameraEnabled ||
        !m_cameraManager.hasActiveCamera())
    {
        return;
    }

    if (m_playerFollowCameraEnabled)
    {
        const CameraManager::Camera& freeCamera = m_cameraManager.camera(m_freeCameraIndex);
        m_followCameraController.snapToCamera(freeCamera, m_playerPawn);
        m_cameraManager.camera(m_playerCameraIndex) = freeCamera;
        m_cameraManager.setActiveCamera(m_playerCameraIndex);
    }
    else
    {
        m_cameraManager.camera(m_freeCameraIndex) = m_cameraManager.camera(m_playerCameraIndex);
        m_cameraManager.setActiveCamera(m_freeCameraIndex);
    }

    m_previousPlayerFollowCameraEnabled = m_playerFollowCameraEnabled;
}

void App::updateSceneForFrame()
{
    HELLO_PROFILE_SCOPE("App::UpdateSceneForFrame");

    if (m_panels.viewportPaused() || !m_cameraManager.hasActiveCamera())
    {
        return;
    }

    const Extent2D viewportExtent = m_panels.viewportExtent();

    {
        HELLO_PROFILE_SCOPE("App::UpdateSceneForFrame::SyncRenderState");
        const Position& cameraPosition = m_cameraManager.activeCameraPosition();
        m_renderer.setViewportSize(viewportExtent);
        m_renderer.setActiveCamera(
            cameraPosition,
            m_triangleRenderer,
            m_quadtreeMeshRenderer,
            m_foliageCanopyRenderer,
            m_foliageRenderer,
            m_nearbyFoliageRenderer,
            m_waterMeshRenderer,
            m_lineRenderer);
        m_quadtreeMeshRenderer.setTerrainHeightParams(
            static_cast<float>(m_worldGridQuadtree.terrainSettings().baseHeight),
            static_cast<float>(terrainNoiseMaxAmplitude(m_worldGridQuadtree.terrainSettings())));
        if constexpr (AppConfig::Foliage::kCanopyEnabled)
        {
            m_foliageCanopyRenderer.setActiveCamera(cameraPosition);
            m_foliageCanopyManager.setTerrainSettings(m_worldGridQuadtree.terrainSettings());
        }
        if constexpr (AppConfig::Foliage::kEnabled)
        {
            m_foliageRenderer.setActiveCamera(cameraPosition);
            m_nearbyFoliageRenderer.setActiveCamera(
                cameraPosition,
                m_cameraManager.activeCamera().forward,
                m_cameraManager.activeCamera().up,
                viewportExtent);
            m_foliageManager.setTerrainSettings(m_worldGridQuadtree.terrainSettings());
        }
        if constexpr (AppConfig::Water::kEnabled)
        {
            m_waterManager.beginFrame();
            m_waterManager.setActiveCamera(cameraPosition);
            const WaterSettings& waterSettings = m_waterManager.settings();
            m_quadtreeMeshRenderer.setWaterCausticsState(waterSettings);
            const float expectedWaveHeight =
                (waterSettings.globalAmplitude * AppConfig::Water::kExpectedWaveHeight) +
                AppConfig::Water::kVisibilityHeightPadding;
            m_worldGridQuadtree.setWaterVisibilityBounds(
                waterSettings.waterLevel - expectedWaveHeight,
                waterSettings.waterLevel + expectedWaveHeight,
                waterSettings.enabled);
            if constexpr (AppConfig::Foliage::kEnabled)
            {
                m_foliageManager.setWaterLevel(waterSettings.waterLevel);
            }
            if constexpr (AppConfig::Foliage::kCanopyEnabled)
            {
                m_foliageCanopyManager.setWaterLevel(waterSettings.waterLevel);
            }
        }
        else
        {
            WaterSettings disabledWaterSettings{};
            disabledWaterSettings.enabled = false;
            disabledWaterSettings.drawTerrainCaustics = false;
            m_quadtreeMeshRenderer.setWaterCausticsState(disabledWaterSettings);
            m_worldGridQuadtree.setWaterVisibilityBounds(0.0f, 0.0f, false);
            if constexpr (AppConfig::Foliage::kEnabled)
            {
                m_foliageManager.setWaterLevel(0.0f);
            }
            if constexpr (AppConfig::Foliage::kCanopyEnabled)
            {
                m_foliageCanopyManager.setWaterLevel(0.0f);
            }
        }
    }

    {
        HELLO_PROFILE_SCOPE("App::UpdateSceneForFrame::BeginHeightmapUpdate");
        m_worldGridQuadtree.beginHeightmapUpdate(m_quadtreeMeshRenderer);
    }

    if constexpr (AppConfig::Foliage::kEnabled)
    {
        HELLO_PROFILE_SCOPE("App::UpdateSceneForFrame::CollectFoliageReadbacks");
        m_nearbyFoliageRenderer.collectCompletedDecodedPages();
        m_nearbyFoliageRenderer.beginFrame(m_frameIndex);
        std::vector<QuadtreeMeshRenderer::GeneratedFoliagePageLiveCount> completedLiveCounts;
        m_quadtreeMeshRenderer.collectCompletedFoliagePageLiveCounts(completedLiveCounts);
        std::vector<std::pair<WorldGridQuadtreeLeafId, std::uint16_t>> generatedLiveCounts;
        generatedLiveCounts.reserve(completedLiveCounts.size());
        for (const QuadtreeMeshRenderer::GeneratedFoliagePageLiveCount& generated : completedLiveCounts)
        {
            generatedLiveCounts.emplace_back(generated.leafId, generated.liveCount);
        }
        m_foliageManager.applyGeneratedPageLiveCounts(generatedLiveCounts);
    }

    if (m_playerFollowCameraEnabled)
    {
        HELLO_PROFILE_SCOPE("App::UpdateSceneForFrame::UpdateGameplay");
        m_cameraManager.setActiveCamera(m_playerCameraIndex);
        m_collisionManager.updateAroundPlayer(
            m_playerPawn.position,
            m_frameIndex,
            m_worldGridQuadtree.heightmapManager(),
            m_foliageManager,
            m_nearbyFoliageRenderer,
            m_quadtreeMeshRenderer);
        m_characterMotor.update(
            m_playerPawn,
            m_playerMoveIntent,
            m_cameraManager.activeCamera(),
            m_collisionManager,
            m_deltaTimeSeconds);
        m_followCameraController.update(
            m_cameraManager.activeCamera(),
            m_playerPawn,
            m_collisionManager,
            m_playerMoveIntent,
            m_deltaTimeSeconds);
    }

    if (m_playerFollowCameraEnabled)
    {
        HELLO_PROFILE_SCOPE("App::UpdateSceneForFrame::SyncFollowCameraRenderState");
        const Position& cameraPosition = m_cameraManager.activeCameraPosition();
        m_renderer.setActiveCamera(
            cameraPosition,
            m_triangleRenderer,
            m_quadtreeMeshRenderer,
            m_foliageCanopyRenderer,
            m_foliageRenderer,
            m_nearbyFoliageRenderer,
            m_waterMeshRenderer,
            m_lineRenderer);
        if constexpr (AppConfig::Foliage::kCanopyEnabled)
        {
            m_foliageCanopyRenderer.setActiveCamera(cameraPosition);
        }
        if constexpr (AppConfig::Foliage::kEnabled)
        {
            m_foliageRenderer.setActiveCamera(cameraPosition);
            m_nearbyFoliageRenderer.setActiveCamera(
                cameraPosition,
                m_cameraManager.activeCamera().forward,
                m_cameraManager.activeCamera().up,
                viewportExtent);
        }
        if constexpr (AppConfig::Water::kEnabled)
        {
            m_waterManager.setActiveCamera(cameraPosition);
        }
    }

    {
        HELLO_PROFILE_SCOPE("App::UpdateSceneForFrame::BuildTriangles");
        m_triangleRenderer.clear();
        m_quadtreeMeshRenderer.clear();
        if constexpr (AppConfig::Foliage::kCanopyEnabled)
        {
            m_foliageCanopyRenderer.clear();
        }
        if constexpr (AppConfig::Foliage::kEnabled)
        {
            m_foliageRenderer.clear();
        }
        for (const TriangleInstance& instance : m_instances)
        {
            m_triangleRenderer.addTriangle(instance.position);
        }
        if (m_playerFollowCameraEnabled)
        {
            m_triangleRenderer.addTriangle(
                m_playerPawn.position.translated({ 0.0, 1.0, 0.0 }),
                static_cast<float>(m_playerPawn.yawRadians));
        }
    }

    {
        HELLO_PROFILE_SCOPE("App::UpdateSceneForFrame::BuildDebugAxes");
        m_lineRenderer.clear();
        const Position origin(0, 0, { 0.0, 0.0, 0.0 });
        const Position axisX(0, 0, { 1.0, 0.0, 0.0 });
        const Position axisY(0, 0, { 0.0, 1.0, 0.0 });
        const Position axisZ(0, 0, { 0.0, 0.0, 1.0 });
        m_lineRenderer.addLine(origin, axisX, glm::vec3(1.0f, 0.25f, 0.25f));
        m_lineRenderer.addLine(origin, axisY, glm::vec3(0.25f, 1.0f, 0.25f));
        m_lineRenderer.addLine(origin, axisZ, glm::vec3(0.35f, 0.6f, 1.0f));
    }

    {
        HELLO_PROFILE_SCOPE("App::UpdateSceneForFrame::UpdateQuadtree");
        m_worldGridQuadtree.updateTree(
            m_cameraManager.activeCamera(),
            viewportExtent,
            AppConfig::Foliage::kEnabled ? &m_foliageManager : nullptr,
            AppConfig::Foliage::kCanopyEnabled ? &m_foliageCanopyManager : nullptr,
            AppConfig::Foliage::kEnabled ? &m_nearbyFoliageRenderer : nullptr,
            m_frameIndex);
    }

    {
        HELLO_PROFILE_SCOPE("App::UpdateSceneForFrame::EndHeightmapUpdate");
        m_worldGridQuadtree.endHeightmapUpdate(m_quadtreeMeshRenderer);
        if constexpr (AppConfig::Foliage::kCanopyEnabled)
        {
            m_foliageCanopyManager.scheduleQueuedGenerations(m_foliageCanopyRenderer);
        }
        if constexpr (AppConfig::Foliage::kEnabled)
        {
            m_foliageManager.scheduleQueuedGenerations(m_quadtreeMeshRenderer);
        }
    }

    RenderEngines renderEngines{
        .triangleRenderer = m_triangleRenderer,
        .lineRenderer = m_lineRenderer,
        .quadtreeMeshRenderer = &m_quadtreeMeshRenderer,
    };

    {
        HELLO_PROFILE_SCOPE("App::UpdateSceneForFrame::EmitSceneDraws");
        m_worldGridQuadtree.emitSceneDraws(
            renderEngines,
            AppConfig::Foliage::kEnabled ? &m_foliageManager : nullptr,
            AppConfig::Foliage::kCanopyEnabled ? &m_foliageCanopyManager : nullptr,
            AppConfig::Foliage::kEnabled ? &m_foliageRenderer : nullptr,
            AppConfig::Foliage::kEnabled ? &m_nearbyFoliageRenderer : nullptr,
            AppConfig::Foliage::kCanopyEnabled ? &m_foliageCanopyRenderer : nullptr,
            AppConfig::Water::kEnabled ? &m_waterManager : nullptr);
        if constexpr (AppConfig::Water::kEnabled)
        {
            m_waterManager.flushToRenderer(m_waterMeshRenderer);
        }
    }

    if (m_panels.showQuadtreeBorders())
    {
        HELLO_PROFILE_SCOPE_GROUPS(
            "App::UpdateSceneForFrame::EmitDebugDraws",
            ProfileScopeGroup::TreeUpdate);
        m_worldGridQuadtree.emitDebugDraws(renderEngines);
    }
}

std::vector<std::string> App::querySdlGpuDrivers() const
{
    std::vector<std::string> drivers;
    const int count = SDL_GetNumGPUDrivers();
    drivers.reserve(static_cast<std::size_t>(std::max(count, 0)));

    for (int index = 0; index < count; ++index)
    {
        const char* name = SDL_GetGPUDriver(index);
        if (name != nullptr)
        {
            drivers.emplace_back(name);
        }
    }

    return drivers;
}
