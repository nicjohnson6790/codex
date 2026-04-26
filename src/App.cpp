#include "App.hpp"

#include "AppConfig.hpp"
#include <SDL3/SDL_gpu.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <algorithm>
#include <filesystem>
#include <glm/vec3.hpp>
#include <iostream>
#include <stdexcept>
#include <string_view>

App::App(const Options& options)
    : m_options(options)
{
}

void App::logStartup(std::string_view message) const
{
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
        m_lastFrameTsc = frameStartTsc;

        performanceCapture.setPaused(m_panels.viewportPaused());
        performanceCapture.beginFrame();

        pollEvents();

        const GamepadState gamepadState = m_gamepadInput.pollState();

        {
            HELLO_PROFILE_SCOPE("App::UpdateCamera");
            if (!m_panels.viewportPaused() && m_cameraManager.hasActiveCamera())
            {
                m_cameraController.update(m_cameraManager.activeCamera(), gamepadState, deltaTimeSeconds);
            }
        }

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        {
            HELLO_PROFILE_SCOPE("App::BuildUi");
            buildUi();
        }

        {
            HELLO_PROFILE_SCOPE("App::UpdateScene");
            updateSceneForFrame();
        }

        ImGui::Render();
        m_renderer.renderFrame(
            m_triangleRenderer,
            m_quadtreeMeshRenderer,
            m_lineRenderer,
            m_cameraManager.buildActiveViewProjectionMatrix(m_panels.viewportExtent()),
            m_lightingSystem,
            ImGui::GetDrawData(),
            !m_panels.viewportPaused()
        );

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
        "SDL3 GPU ImGui Triangle",
        1440,
        900,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );
    if (m_window == nullptr)
    {
        throw std::runtime_error(SDL_GetError());
    }

    logStartup("query SDL GPU drivers");
    m_gpuDrivers = querySdlGpuDrivers();
    logStartup("init gamepad input");
    m_gamepadInput.initialize();
    logStartup("init performance capture");
    PerformanceCapture::instance().initialize(AppConfig::Perf::kHistorySeconds);
    logStartup("create default camera");
    m_cameraManager.createCamera(
        "Camera 1",
        Position(0, 0, { 0.0, 0.0, 2.8 })
    );

    logStartup("init SDL GPU renderer");
    m_renderer.initialize(m_window);

    const std::filesystem::path shaderDirectory = HELLO_TRIANGLE_SHADER_DIR;
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
        .instances = m_instances,
        .gpuDrivers = m_gpuDrivers,
        .gamepadName = m_gamepadInput.gamepadName(),
        .lightingSystem = m_lightingSystem,
        .worldGridQuadtree = m_worldGridQuadtree,
        .viewportTextureId = m_renderer.viewportTextureId(),
    };
    m_panels.draw(context);
}

void App::updateSceneForFrame()
{
    if (m_panels.viewportPaused() || !m_cameraManager.hasActiveCamera())
    {
        return;
    }

    const Extent2D viewportExtent = m_panels.viewportExtent();
    const Position& cameraPosition = m_cameraManager.activeCameraPosition();

    m_renderer.setViewportSize(viewportExtent);
    m_renderer.setActiveCamera(cameraPosition, m_triangleRenderer, m_quadtreeMeshRenderer, m_lineRenderer);
    m_quadtreeMeshRenderer.setTerrainHeightParams(
        static_cast<float>(m_worldGridQuadtree.terrainSettings().baseHeight),
        AppConfig::Terrain::kHeightAmplitude);

    m_triangleRenderer.clear();
    m_quadtreeMeshRenderer.clear();
    for (const TriangleInstance& instance : m_instances)
    {
        m_triangleRenderer.addTriangle(instance.position);
    }

    m_lineRenderer.clear();
    const Position origin(0, 0, { 0.0, 0.0, 0.0 });
    const Position axisX(0, 0, { 1.0, 0.0, 0.0 });
    const Position axisY(0, 0, { 0.0, 1.0, 0.0 });
    const Position axisZ(0, 0, { 0.0, 0.0, 1.0 });
    m_lineRenderer.addLine(origin, axisX, glm::vec3(1.0f, 0.25f, 0.25f));
    m_lineRenderer.addLine(origin, axisY, glm::vec3(0.25f, 1.0f, 0.25f));
    m_lineRenderer.addLine(origin, axisZ, glm::vec3(0.35f, 0.6f, 1.0f));

    m_worldGridQuadtree.updateTree(m_cameraManager.activeCamera(), viewportExtent);

    RenderEngines renderEngines{
        .triangleRenderer = m_triangleRenderer,
        .lineRenderer = m_lineRenderer,
        .quadtreeMeshRenderer = &m_quadtreeMeshRenderer,
    };
    m_worldGridQuadtree.emitMeshDraws(renderEngines);
    if (m_panels.showQuadtreeBorders())
    {
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
