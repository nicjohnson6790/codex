#include "AppPanels.hpp"

#include "AppConfig.hpp"
#include "PerformanceCapture.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <string>

void AppPanels::draw(Context &context)
{
    HELLO_PROFILE_SCOPE_GROUPS("AppPanels::Draw", ProfileScopeGroup::ImGui);
    drawDockSpace();
    drawInfoPane(context);
    drawViewportPane(context);
}

void AppPanels::drawDockSpace()
{
    HELLO_PROFILE_SCOPE("AppPanels::DrawDockSpace");
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                       ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("DockSpaceHost", nullptr, flags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
    ImGui::DockSpace(dockspaceId);
    applyDockLayout();
    ImGui::End();
}

void AppPanels::applyDockLayout()
{
    if (m_dockLayoutInitialized && !m_layoutDirty)
    {
        return;
    }

    ImGuiViewport *mainViewport = ImGui::GetMainViewport();
    const ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");

    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, mainViewport->Size);

    if (m_leftPaneCollapsed)
    {
        m_viewportDockId = dockspaceId;
        ImGui::DockBuilderDockWindow("Viewport", dockspaceId);
    }
    else
    {
        ImGuiID infoDockId = dockspaceId;
        ImGuiID viewportDockId = 0;
        ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.28f, &infoDockId, &viewportDockId);
        m_viewportDockId = viewportDockId;

        ImGui::DockBuilderDockWindow("Info", infoDockId);
        ImGui::DockBuilderDockWindow("Viewport", viewportDockId);
    }

    ImGui::DockBuilderFinish(dockspaceId);

    m_dockLayoutInitialized = true;
    m_layoutDirty = false;
}

void AppPanels::drawInfoPane(Context &context)
{
    HELLO_PROFILE_SCOPE("AppPanels::DrawInfoPane");
    if (m_leftPaneCollapsed)
    {
        return;
    }

    ImGui::Begin("Info");

    if (ImGui::BeginTabBar("InfoTabs"))
    {
        if (ImGui::BeginTabItem("Controls"))
        {
            drawControlsTab(context);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Perf"))
        {
            m_perfPanel.draw(m_viewportPaused);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Steam"))
        {
            drawSteamTab(context);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("MP"))
        {
            drawMultiplayerTab(context);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Terrain"))
        {
            drawTerrainTab(context);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Water"))
        {
            drawWaterTab(context);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Debug"))
        {
            drawDebugTab(context);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}

void AppPanels::drawControlsTab(Context &context)
{
    HELLO_PROFILE_SCOPE("AppPanels::DrawControlsTab");
    ImGui::TextUnformatted("Stack");
    ImGui::Separator();
    ImGui::Text("SDL3 window + SDL GPU renderer + Dear ImGui");
    ImGui::Text("Viewport texture size: %u x %u", m_viewportPanelExtent.width, m_viewportPanelExtent.height);
    ImGui::Text("Gamepad: %.*s", static_cast<int>(context.gamepadName.size()), context.gamepadName.data());
    ImGui::Checkbox("Show viewport FPS overlay", &m_showViewportFpsCounter);
    bool vsyncEnabled = context.renderer.vsyncEnabled();
    if (ImGui::Checkbox("VSync", &vsyncEnabled))
    {
        context.renderer.setVsyncEnabled(vsyncEnabled);
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("SDL GPU Drivers");
    ImGui::Separator();
    for (const std::string &driver : context.gpuDrivers)
    {
        ImGui::BulletText("%s", driver.c_str());
    }

    const CameraManager::Camera &activeCamera = context.cameraManager.activeCamera();
    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("Active: %s", activeCamera.name.c_str());
        ImGui::Text("Camera Cell: (%lld, %lld)", static_cast<long long>(activeCamera.position.gridX()),
                    static_cast<long long>(activeCamera.position.gridY()));
        const glm::dvec3 &cameraLocal = activeCamera.position.localPosition();
        ImGui::Text("Camera Local: (%.3f, %.3f, %.3f)", cameraLocal.x, cameraLocal.y, cameraLocal.z);
        if (ImGui::Button("Add Camera"))
        {
            const std::size_t newCameraIndex = context.cameraManager.cameraCount() + 1;
            context.cameraManager.createCamera("Camera " + std::to_string(newCameraIndex), activeCamera.position, activeCamera.forward,
                                               activeCamera.up);
        }
        ImGui::SameLine();
        if (ImGui::Button("Next Camera") && context.cameraManager.cameraCount() > 0)
        {
            context.cameraManager.setActiveCamera((context.cameraManager.activeCameraIndex() + 1) % context.cameraManager.cameraCount());
        }

        ImGui::SeparatorText("Gamepad");
        ImGui::BulletText("Left stick: strafe / move forward-back");
        ImGui::BulletText("Right stick: yaw / pitch");
        ImGui::BulletText("Triggers: move up / down");
        ImGui::BulletText("LB / RB: roll");
        ImGui::BulletText("Press right stick: align camera up to world up");
    }

    if (ImGui::CollapsingHeader("Player", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Use player follow camera", &context.playerFollowCameraEnabled);
        const glm::dvec3 playerWorld = context.playerPawn.position.worldPosition();
        ImGui::Text("Player world: (%.2f, %.2f, %.2f)", playerWorld.x, playerWorld.y, playerWorld.z);
        ImGui::Text("Grounded: %s", context.playerPawn.grounded ? "yes" : "no");
        ImGui::Text("Collision tiles ready: %u / 16", context.collisionManager.readyTileCount());
        ImGui::SeparatorText("Keyboard");
        ImGui::BulletText("WASD: move");
        ImGui::BulletText("Shift: sprint");
        ImGui::SeparatorText("Gamepad");
        ImGui::BulletText("Left stick: move");
        ImGui::BulletText("Right stick: orbit camera");
        ImGui::BulletText("RB: sprint");
    }

    if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen))
    {
        LightingSystem::SunLight &sun = context.lightingSystem.sun();
        ImGui::SliderFloat("Azimuth", &sun.azimuthDegrees, -180.0f, 180.0f, "%.1f deg");
        ImGui::SliderFloat("Elevation", &sun.elevationDegrees, 1.0f, 89.0f, "%.1f deg");
        ImGui::SliderFloat("Time of day", &sun.timeOfDayHours, 0.0f, 24.0f, "%.2f h");
        ImGui::InputFloat("Day length (s)", &sun.dayLengthSeconds, 1.0f, 10.0f, "%.1f");
        sun.dayLengthSeconds = std::max(sun.dayLengthSeconds, 0.1f);
        ImGui::InputFloat("Time factor", &sun.timeFactor, 0.1f, 1.0f, "%.2f");
        ImGui::InputFloat3("Linear sun radiance RGB", &sun.color.x);
        ImGui::SliderFloat("Intensity", &sun.intensity, 0.0f, 4.0f, "%.2f");
    }

    ImGui::SeparatorText("Viewport display");
    context.renderer.displayTransform().drawSettings();
    context.cloudRenderer.drawSettings();
    if (ImGui::CollapsingHeader("Atmosphere", ImGuiTreeNodeFlags_DefaultOpen))
    {
        SkyboxRenderer::AtmosphereSettings &atmosphere = context.skyboxRenderer.atmosphereSettings();

        ImGui::InputFloat("Atmosphere height (m)", &atmosphere.atmosphereHeight, 1000.0f, 10000.0f, "%.0f");
        ImGui::InputFloat("Distance range (m)", &atmosphere.atmosphereDistanceRange, 10000.0f, 100000.0f, "%.0f");
        ImGui::InputFloat("Atmosphere/cloud source scale", &atmosphere.atmosphereCloudSourceScale, 0.1f, 0.5f, "%.2f");

        if (ImGui::TreeNode("Scattering"))
        {
            ImGui::InputFloat("Rayleigh R", &atmosphere.rayleighScatterR, 0.1e-6f, 1.0e-6f, "%.6e");
            ImGui::InputFloat("Rayleigh G", &atmosphere.rayleighScatterG, 0.1e-6f, 1.0e-6f, "%.6e");
            ImGui::InputFloat("Rayleigh B", &atmosphere.rayleighScatterB, 0.1e-6f, 1.0e-6f, "%.6e");
            ImGui::InputFloat("Mie scatter", &atmosphere.mieScatter, 0.1e-6f, 1.0e-6f, "%.6e");
            ImGui::InputFloat("Mie extinction", &atmosphere.mieExtinction, 0.1e-6f, 1.0e-6f, "%.6e");
            ImGui::InputFloat("Mie g", &atmosphere.mieG, 0.01f, 0.05f, "%.2f");
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Absorption"))
        {
            ImGui::InputFloat("Ozone R", &atmosphere.ozoneAbsorptionR, 0.01e-6f, 0.1e-6f, "%.6e");
            ImGui::InputFloat("Ozone G", &atmosphere.ozoneAbsorptionG, 0.01e-6f, 0.1e-6f, "%.6e");
            ImGui::InputFloat("Ozone B", &atmosphere.ozoneAbsorptionB, 0.01e-6f, 0.1e-6f, "%.6e");
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Scale Heights"))
        {
            ImGui::InputFloat("Rayleigh scale H", &atmosphere.rayleighScaleHeight, 100.0f, 1000.0f, "%.0f");
            ImGui::InputFloat("Mie scale H", &atmosphere.mieScaleHeight, 50.0f, 250.0f, "%.0f");
            ImGui::InputFloat("Ozone column H", &atmosphere.ozoneColumnHeight, 500.0f, 2500.0f, "%.0f");
            ImGui::TreePop();
        }

        auto& waterMedium = context.skyboxRenderer.waterMediumSettings();
        ImGui::SeparatorText("Underwater medium (inverse meters)");
        ImGui::InputFloat("Water medium source scale", &waterMedium.sourceScale, 0.1f, 0.5f, "%.2f");
        ImGui::InputFloat3("Water absorption RGB", &waterMedium.absorption.x, "%.4f");
        ImGui::InputFloat3("Water scattering RGB", &waterMedium.scattering.x, "%.4f");
        if (ImGui::Button("Reset Underwater Defaults")) waterMedium = {};

        context.skyboxRenderer.sanitizeAtmosphereSettings();

        if (ImGui::Button("Reset Atmosphere Defaults"))
        {
            context.skyboxRenderer.resetAtmosphereSettings();
        }
    }
}

void AppPanels::drawSteamTab(Context &context)
{
    HELLO_PROFILE_SCOPE("AppPanels::DrawSteamTab");

    ImGui::SeparatorText("Runtime");
    ImGui::Text("SDK build: %s", context.steamService.compiledIn() ? "enabled" : "disabled");
    ImGui::Text("Launch request: %s", context.steamService.requested() ? "enabled" : "disabled");
    ImGui::Text("API status: %s", context.steamService.initialized() ? "initialized" : "unavailable");
    ImGui::Text("Steam Input: %s", context.steamService.steamInputInitialized() ? "initialized" : "unavailable");
    ImGui::Text("Input manifest: %s", context.steamService.steamInputManifestLoaded() ? "loaded" : "unavailable");
    ImGui::Text("Action handles: %s", context.steamService.steamInputActionsReady() ? "ready" : "missing");
    ImGui::Text("Input controllers: %d", context.steamService.steamInputControllerCount());
    ImGui::Text("Active input handle: %llu", static_cast<unsigned long long>(context.steamService.activeInputHandle()));
    ImGui::Text("Action data: %s", context.steamService.lastSteamInputStateActive() ? "active" : "inactive");
    ImGui::Text("Move: %.3f, %.3f", context.steamService.lastMoveX(), context.steamService.lastMoveY());
    ImGui::Text("Look: %.3f, %.3f", context.steamService.lastLookX(), context.steamService.lastLookY());

    if (!context.steamService.initialized())
    {
        ImGui::TextWrapped("Steam API is optional for local runs. It initializes when Steam is "
                           "available and the local AppID/runtime setup is valid.");
        return;
    }

    ImGui::SeparatorText("Identity");
    ImGui::Text("AppID: %u", context.steamService.appId());
    ImGui::Text("User ID: %llu", static_cast<unsigned long long>(context.steamService.userId()));
    ImGui::Text("Persona: %s", context.steamService.personaName().c_str());
}

void AppPanels::drawMultiplayerTab(Context &context)
{
    HELLO_PROFILE_SCOPE("AppPanels::DrawMultiplayerTab");

    const MultiplayerManager::DebugSnapshot debug = context.multiplayerManager.debugSnapshot();
    ImGui::SeparatorText("Steam");
    ImGui::Text("Available: %s", context.steamService.initialized() ? "yes" : "no");
    ImGui::Text("Persona: %s", context.steamService.personaName().empty() ? "offline" : context.steamService.personaName().c_str());
    ImGui::Text("Steam ID: %llu", static_cast<unsigned long long>(context.steamService.userId()));

    ImGui::SeparatorText("Session");
    ImGui::Text("State: %s", MultiplayerManager::stateName(debug.state));
    if (context.steamService.lobbyRequestPending())
    {
        ImGui::TextUnformatted("Lobby request pending...");
    }
    if (!debug.error.empty())
    {
        ImGui::TextWrapped("Error: %s", debug.error.c_str());
    }

    if (debug.state == MultiplayerManager::SessionState::Offline || debug.state == MultiplayerManager::SessionState::Error)
    {
        if (ImGui::Button("Create Lobby"))
        {
            m_pendingMultiplayerCommand = MultiplayerCommand::CreateLobby;
            m_pendingLobbyId = 0;
        }
    }
    else
    {
        if (ImGui::Button("Leave Lobby"))
        {
            m_pendingMultiplayerCommand = MultiplayerCommand::LeaveLobby;
            m_pendingLobbyId = 0;
        }
    }

    ImGui::Text("Lobby ID: %llu", static_cast<unsigned long long>(debug.lobbyId));
    ImGui::Text("Host ID: %llu", static_cast<unsigned long long>(debug.hostId));
    ImGui::Text("Connections: %zu", debug.connectionCount);
    ImGui::Text("Remote entities: %zu", debug.remoteEntityCount);
    ImGui::Text("Player packets sent/recv: %llu / %llu", static_cast<unsigned long long>(debug.sentPlayerStateCount),
                static_cast<unsigned long long>(debug.receivedPlayerStateCount));
    ImGui::Text("Packet age: %.2f s", debug.newestPacketAgeSeconds);
    ImGui::Text("Connection: %s", debug.connectionState.c_str());
    ImGui::Text("Time drift: %.4f h", debug.timeSyncDriftHours);
    ImGui::Text("Time revision: %u", debug.timeRevision);

    ImGui::SeparatorText("Friends");
    const std::vector<SteamService::FriendLobby> friendLobbies = context.steamService.joinableFriendLobbies();
    if (friendLobbies.empty())
    {
        ImGui::TextUnformatted("No joinable friends found.");
    }
    for (const SteamService::FriendLobby &friendLobby : friendLobbies)
    {
        ImGui::PushID(static_cast<int>(friendLobby.friendSteamId & 0x7fffffffu));
        ImGui::Text("%s", friendLobby.personaName.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Join"))
        {
            m_pendingMultiplayerCommand = MultiplayerCommand::JoinLobby;
            m_pendingLobbyId = friendLobby.lobbyId;
        }
        ImGui::PopID();
    }

    ImGui::SeparatorText("Members");
    for (const SteamService::LobbyMember &member : context.steamService.lobbyMembers())
    {
        ImGui::BulletText("%s (%llu)%s", member.personaName.c_str(), static_cast<unsigned long long>(member.steamId),
                          member.steamId == debug.hostId ? " host" : "");
    }
}

void AppPanels::drawTerrainTab(Context &context)
{
    HELLO_PROFILE_SCOPE("AppPanels::DrawTerrainTab");

    int computeDispatchBudget = static_cast<int>(context.worldGridQuadtree.computeDispatchBudget());

    ImGui::TextWrapped("Terrain height is composed from tiled ETOPO source heightmaps.");
    ImGui::Spacing();
    ImGui::SliderInt("Final generations/frame", &computeDispatchBudget, 1,
                     static_cast<int>(AppConfig::Terrain::kMaxFinalHeightmapsPerDispatch));
    context.worldGridQuadtree.setComputeDispatchBudget(static_cast<std::uint16_t>(std::max(computeDispatchBudget, 1)));

    ImGui::Spacing();
    if (ImGui::Button("Regenerate Terrain Cache"))
    {
        context.worldGridQuadtree.clearTerrainCache();
    }

    ImGui::Spacing();
    ImGui::Text("Resident slices: %u", context.worldGridQuadtree.residentCount());
    ImGui::Text("Queued leaves: %u", context.worldGridQuadtree.queuedCount());
}

void AppPanels::drawDebugTab(Context &context)
{
    HELLO_PROFILE_SCOPE("AppPanels::DrawDebugTab");
    const WorldGridQuadtree::TreeData &treeData = context.worldGridQuadtree.treeData;

    ImGui::SeparatorText("Quadtree");
    ImGui::Checkbox("Show quadtree borders", &m_showQuadtreeBorders);
    ImGui::Text("Generated cells: 9");
    ImGui::Text("Drawable nodes: %u", treeData.drawableNodeCount);
    ImGui::Text("Started subdividing this frame: %u", treeData.subdivisionCountThisFrame);
    ImGui::Text("Started collapsing this frame: %u", treeData.collapseCountThisFrame);
    ImGui::Text("Max depth: %u", treeData.maxDepth);
    ImGui::Text("Minimum quad size: %.0f", AppConfig::Quadtree::kMinimumQuadSize);

    ImGui::SeparatorText("Terrain Draw Sizes");
    bool showedTerrainDrawSize = false;
    for (std::size_t scalePow = 0; scalePow < treeData.terrainDrawCountByScalePow.size(); ++scalePow)
    {
        const std::uint16_t drawCount = treeData.terrainDrawCountByScalePow[scalePow];
        const std::uint16_t totalLeafCount = treeData.terrainLeafCountByScalePow[scalePow];
        if (drawCount == 0 && totalLeafCount == 0)
        {
            continue;
        }

        const double drawSizeMeters = AppConfig::Quadtree::kMinimumQuadSize * static_cast<double>(std::uint64_t{1} << scalePow);
        ImGui::Text("%.0f m: %u/%u", drawSizeMeters, drawCount, totalLeafCount);
        showedTerrainDrawSize = true;
    }
    if (!showedTerrainDrawSize)
    {
        ImGui::TextUnformatted("No terrain draws submitted");
    }

    ImGui::SeparatorText("Heightmap Cache");
    ImGui::Text("Resident slices: %u", context.worldGridQuadtree.residentCount());
    ImGui::Text("Queued leaves: %u", context.worldGridQuadtree.queuedCount());
    const auto heightmaps = context.worldGridQuadtree.heightmapDiagnostics();
    ImGui::Text("CPU heightmaps: %u / %u (ready %u, loading %u)", heightmaps.cpuOccupied, heightmaps.cpuCapacity,
                heightmaps.cpuReady, heightmaps.cpuLoading);
    ImGui::Text("CPU misses / evictions: %llu / %llu", (unsigned long long)heightmaps.cpuMisses, (unsigned long long)heightmaps.cpuEvictions);
    ImGui::Text("Readbacks: %u / %u (high-water %u)", heightmaps.readbacks, heightmaps.readbackCapacity, heightmaps.readbackHighWater);
    ImGui::Text("CPU cache / readback blocked: %llu / %llu", (unsigned long long)heightmaps.cpuCacheBlocked, (unsigned long long)heightmaps.readbackBlocked);
    ImGui::Text("Source decode staging: %s (256 KiB)", heightmaps.sourceStagingBusy ? "busy" : "available");
    ImGui::Text("Source decode / upload batch high-water: %u / %u", heightmaps.sourceDecodeBatchHighWater,
                heightmaps.sourceUploadBatchHighWater);
    ImGui::Text("Source cache / staging / upload blocked: %llu / %llu / %llu", (unsigned long long)heightmaps.sourceCacheBlocked,
                (unsigned long long)heightmaps.sourceStagingBlocked, (unsigned long long)heightmaps.sourceUploadBlocked);
    ImGui::Text("References per final high-water / overflows: %u / %llu", heightmaps.referenceHighWater,
                (unsigned long long)heightmaps.referenceOverflows);
    ImGui::Text("Source Tile Cache: %u / %u", heightmaps.sourceOccupied, WorldGridQuadtreeHeightmapManager::kSourceTileCapacity);
    ImGui::Text("Ready / loading / age-0: %u / %u / %u", heightmaps.sourceReady, heightmaps.sourceLoading, heightmaps.sourceAgeZero);
    ImGui::Text("Hash LUT: %u / %u, depth %u, collisions %u", heightmaps.sourceHashOccupied, heightmaps.sourceHashCapacity,
                AppConfig::Terrain::kSourceHeightmapHashLookupDepth, heightmaps.sourceHashCollisions);
    ImGui::Text("Hits / misses: %llu / %llu", static_cast<unsigned long long>(heightmaps.sourceHits),
                static_cast<unsigned long long>(heightmaps.sourceMisses));
    ImGui::Text("Loads / uploads / evictions: %llu / %llu / %llu", static_cast<unsigned long long>(heightmaps.sourceLoads),
                static_cast<unsigned long long>(heightmaps.sourceUploads), static_cast<unsigned long long>(heightmaps.sourceEvictions));
    ImGui::Text("Source References: %u / %u (high-water %u)", heightmaps.referenceCount, heightmaps.referenceCapacity,
                heightmaps.referenceHighWater);
    ImGui::Text("Waiting finals / contributions: %u / %u", heightmaps.waitingFinals, heightmaps.pendingContributions);
    ImGui::Text("Source Descriptors: %u / %u", heightmaps.lastSourceDescriptors,
                WorldGridQuadtreeHeightmapManager::kSourceDescriptorCapacity);
    ImGui::Text("Final Generations: %u / %u", heightmaps.lastFinalGenerations,
                WorldGridQuadtreeHeightmapManager::kMaxFinalHeightmapsPerDispatch);
    ImGui::Text("Queued / submitted generation jobs: %u / %u", heightmaps.queuedGenerationJobs, heightmaps.submittedJobs);
    ImGui::Text("Submitted / discarded / descriptor errors: %u / %u / %u", heightmaps.submittedJobs, heightmaps.discardedJobs,
                heightmaps.descriptorOverflows);
    ImGui::Text("Completed final generations: %llu", static_cast<unsigned long long>(heightmaps.completedFinalGenerations));
    ImGui::Text("Completed with source contributions: %llu",
                static_cast<unsigned long long>(heightmaps.completedFinalGenerationsWithSources));

    ImGui::SeparatorText("Foliage Overview");
    ImGui::Text("Canopy draws: %u", context.foliageCanopyRenderer.drawCount());
    ImGui::Text("Emitted foliage page draws: %u", context.foliageRenderer.drawCount());
    ImGui::Text("Emitted foliage marker instances: %u", context.foliageRenderer.emittedInstanceCount());
    ImGui::Text("Nearby foliage draw calls: %u", context.nearbyFoliageRenderer.drawCallCount());
    ImGui::Text("Nearby foliage marker instances: %u", context.nearbyFoliageRenderer.emittedInstanceCount());

    ImGui::SeparatorText("Foliage GPU Residency");
    ImGui::Text("Resident foliage pages: %u", context.foliageManager.residentCount());
    ImGui::Text("Queued foliage pages: %u", context.foliageManager.queuedCount());
    ImGui::Text("GPU page-generation pending: %u", context.foliageManager.maskPendingCount());
    ImGui::Text("GPU generation pending pages: %u", context.foliageManager.uploadPendingCount());
    ImGui::Text("Ready foliage pages: %u", context.foliageManager.readyCount());
    ImGui::Text("Foliage page-pool capacity: %u", FoliageConfig::kPagePoolCapacity);

    ImGui::SeparatorText("Nearby Foliage LRU");
    ImGui::Text("Decoded CPU-resident pages: %u", context.nearbyFoliageManager.residentCount());
    ImGui::Text("Decoded-page jobs pending: %u", context.nearbyFoliageManager.pendingCount());
    ImGui::Text("Decoded-page LRU capacity: %u", FoliageConfig::kNearbyDecodedPageLruCapacity);
    ImGui::Text("Nearby radius: %.1f m", FoliageConfig::kNearbyDefaultRadiusMeters);
    ImGui::Text("Candidate slots/page: %u", FoliageConfig::kCandidateSlotCount);

    ImGui::SeparatorText("Canopy Residency");
    ImGui::Text("Resident canopy cells: %u", context.foliageCanopyManager.residentCount());
    ImGui::Text("Queued canopy cells: %u", context.foliageCanopyManager.queuedCount());
    ImGui::Text("Canopy cell-pool capacity: %u", FoliageConfig::kCanopyCellPoolCapacity);

    ImGui::SeparatorText("Water");
    ImGui::Text("Queued water instances: %u", context.waterManager.queuedCount());
    ImGui::Text("Water mesh instances: %u", context.waterMeshRenderer.instanceCount());
    ImGui::Text("Water mesh resolution: %u x %u vertices", AppConfig::Water::kMeshVertexResolution,
                AppConfig::Water::kMeshVertexResolution);

    ImGui::SeparatorText("Notes");
    ImGui::TextWrapped("The quadtree is rebuilt around the active camera and includes the "
                       "current grid cell plus the eight surrounding cells.");
}

void AppPanels::drawWaterTab(Context &context)
{
    HELLO_PROFILE_SCOPE("AppPanels::DrawWaterTab");

    WaterSettings &settings = context.waterManager.settings();
    ImGui::Checkbox("Enabled", &settings.enabled);
    ImGui::Checkbox("Show LOD tint", &settings.showLodTint);
    ImGui::Checkbox("Draw foam", &settings.drawFoam);
    ImGui::Checkbox("Draw terrain caustics", &settings.drawTerrainCaustics);
    ImGui::InputFloat("Water level", &settings.waterLevel, 1.0f, 10.0f, "%.2f");
    ImGui::InputFloat("Global amplitude", &settings.globalAmplitude, 0.05f, 0.25f, "%.2f");
    ImGui::InputFloat("Global choppiness", &settings.globalChoppiness, 0.05f, 0.25f, "%.2f");
    ImGui::InputFloat("Depth (m)", &settings.depthMeters, 1.0f, 10.0f, "%.2f");
    ImGui::InputFloat("Low cutoff", &settings.lowCutoff, 0.0001f, 0.001f, "%.4f");
    ImGui::InputFloat("High cutoff", &settings.highCutoff, 0.1f, 1.0f, "%.2f");
    ImGui::InputFloat("Dry terrain cutoff above water (m)", &settings.maxTerrainMinHeightAboveWaterToDraw, 1.0f, 10.0f, "%.2f");

    int cascadeCount = static_cast<int>(settings.cascadeCount);
    ImGui::SliderInt("Cascade count", &cascadeCount, 0, static_cast<int>(AppConfig::Water::kMaxCascadeCount));
    settings.cascadeCount = static_cast<std::uint32_t>(std::max(cascadeCount, 0));

    if (ImGui::CollapsingHeader("Water Cascades", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (std::uint32_t index = 0; index < settings.cascadeCount; ++index)
        {
            WaterCascadeSettings &cascade = settings.cascades[index];
            ImGui::PushID(static_cast<int>(index));
            ImGui::SeparatorText(("Cascade " + std::to_string(index)).c_str());
            ImGui::InputFloat("World size (m)", &cascade.worldSizeMeters, 1.0f, 10.0f, "%.1f");
            ImGui::InputFloat("Amplitude", &cascade.amplitude, 0.05f, 0.25f, "%.2f");
            ImGui::InputFloat("Wind speed", &cascade.windSpeed, 0.5f, 2.0f, "%.2f");
            ImGui::InputFloat("Wind direction", &cascade.windDirectionRadians, 0.05f, 0.2f, "%.2f");
            ImGui::InputFloat("Fetch (m)", &cascade.fetchMeters, 100.0f, 1000.0f, "%.1f");
            ImGui::SliderFloat("Spread blend", &cascade.spreadBlend, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Swell", &cascade.swell, 0.01f, 1.0f, "%.2f");
            ImGui::InputFloat("Peak enhancement", &cascade.peakEnhancement, 0.1f, 0.5f, "%.2f");
            ImGui::InputFloat("Short waves fade", &cascade.shortWavesFade, 0.0001f, 0.0005f, "%.4f");
            ImGui::InputFloat("Choppiness", &cascade.choppiness, 0.05f, 0.25f, "%.2f");
            ImGui::InputFloat("Shallow damping", &cascade.shallowDampingStrength, 0.05f, 0.25f, "%.2f");
            ImGui::InputFloat("Shallow depth (m)", &cascade.shallowDampingDepthMeters, 0.5f, 2.0f, "%.2f");
            int updateModulo = static_cast<int>(cascade.updateModulo);
            ImGui::SliderInt("Update modulo", &updateModulo, 1, 8);
            cascade.updateModulo = static_cast<std::uint32_t>(std::max(updateModulo, 1));
            ImGui::PopID();
        }
    }

    if (ImGui::CollapsingHeader("Crest Foam", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Enabled##CrestFoam", &settings.crestFoamEnabled);
        ImGui::InputFloat("Amount", &settings.crestFoamAmount, 0.05f, 0.25f, "%.2f");
        ImGui::InputFloat("Compression threshold", &settings.crestFoamThreshold, 0.01f, 0.05f, "%.3f");
        ImGui::InputFloat("Threshold softness", &settings.crestFoamSoftness, 0.01f, 0.05f, "%.3f");
        ImGui::InputFloat("Slope gate start", &settings.crestFoamSlopeStart, 0.01f, 0.05f, "%.2f");
        ImGui::InputFloat("Decay rate", &settings.crestFoamDecayRate, 0.01f, 0.05f, "%.3f");
        ImGui::InputFloat("Brightness", &settings.crestFoamBrightness, 0.05f, 0.25f, "%.2f");
    }

    if (ImGui::CollapsingHeader("Foam Detail", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("Merged foam history picks the fresh-to-decayed ridge shape. A "
                           "world-space smooth-noise sample drives small offsets in both the foam "
                           "history lookup and the foam detail lookup.");
        ImGui::Spacing();
        ImGui::InputFloat("Foam cell size##FoamDetail", &settings.foamSdfSampleScaleA, 0.05f, 0.2f, "%.3f");
        ImGui::InputFloat("Fresh foam ridge min##FoamDetail", &settings.foamSdfRidgeMinA, 0.005f, 0.02f, "%.3f");
        ImGui::InputFloat("Fresh foam ridge max##FoamDetail", &settings.foamSdfRidgeMaxA, 0.005f, 0.02f, "%.3f");
        ImGui::InputFloat("Decayed foam ridge min##FoamDetail", &settings.foamSdfRidgeMinB, 0.005f, 0.02f, "%.3f");
        ImGui::InputFloat("Decayed foam ridge max##FoamDetail", &settings.foamSdfRidgeMaxB, 0.005f, 0.02f, "%.3f");
        ImGui::Spacing();
        ImGui::InputFloat("Smooth-noise world scale##FoamDetail", &settings.foamNoiseScale, 0.01f, 0.05f, "%.3f");
        ImGui::InputFloat("History wobble strength##FoamDetail", &settings.foamHistoryWarpStrength, 0.25f, 1.0f, "%.3f");
        ImGui::InputFloat("Detail wobble strength##FoamDetail", &settings.foamDetailOffsetStrength, 0.005f, 0.02f, "%.3f");
        ImGui::InputFloat("Detail breakup strength##FoamDetail", &settings.foamDetailBreakupStrength, 0.05f, 0.2f, "%.3f");
        ImGui::InputFloat("Detail breakup scale##FoamDetail", &settings.foamDetailBreakupScale, 0.005f, 0.02f, "%.3f");
        ImGui::Spacing();
        ImGui::InputFloat("Fresh-to-decayed start##FoamDetail", &settings.foamEvolutionStart, 0.02f, 0.1f, "%.3f");
        ImGui::InputFloat("Fresh-to-decayed end##FoamDetail", &settings.foamEvolutionEnd, 0.02f, 0.1f, "%.3f");
        ImGui::InputFloat("Decayed-shape dropoff end##FoamDetail", &settings.foamEvolutionDropoffEnd, 0.02f, 0.1f, "%.3f");
        ImGui::InputFloat("Visibility fade start##FoamDetail", &settings.foamFadeStart, 0.001f, 0.005f, "%.3f");
        ImGui::InputFloat("Visibility fade end##FoamDetail", &settings.foamFadeEnd, 0.005f, 0.02f, "%.3f");
    }

    if (ImGui::CollapsingHeader("Shore Foam", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Enabled##ShoreFoam", &settings.shoreFoamEnabled);
        ImGui::InputFloat("Amount##ShoreFoam", &settings.shoreFoamAmount, 0.05f, 0.25f, "%.3f");
        ImGui::InputFloat("Depth start##ShoreFoam", &settings.shoreFoamDepthStart, 0.05f, 0.25f, "%.3f");
        ImGui::InputFloat("Depth end##ShoreFoam", &settings.shoreFoamDepthEnd, 0.10f, 0.50f, "%.3f");
        ImGui::InputFloat("Decay depth start##ShoreFoam", &settings.shoreFoamDecayDepthStart, 0.05f, 0.25f, "%.3f");
        ImGui::InputFloat("Decay depth end##ShoreFoam", &settings.shoreFoamDecayDepthEnd, 0.10f, 0.50f, "%.3f");
        ImGui::InputFloat("Breakup strength##ShoreFoam", &settings.shoreFoamBreakupStrength, 0.05f, 0.25f, "%.3f");
    }

    if (ImGui::CollapsingHeader("Terrain Caustics", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::InputFloat("Intensity", &settings.causticsIntensity, 0.01f, 0.05f, "%.3f");
        ImGui::InputFloat("Sample A scale", &settings.causticsPatternScaleA, 0.01f, 0.05f, "%.3f");
        ImGui::InputFloat("Sample B scale", &settings.causticsPatternScaleB, 0.01f, 0.05f, "%.3f");
        ImGui::InputFloat("Sample A rotation", &settings.causticsRotationA, 0.05f, 0.2f, "%.3f");
        ImGui::InputFloat("Sample B rotation", &settings.causticsRotationB, 0.05f, 0.2f, "%.3f");
        ImGui::InputFloat("Disp warp", &settings.causticsDisplacementWarpStrength, 0.01f, 0.05f, "%.3f");
        ImGui::InputFloat("Slope warp", &settings.causticsSlopeWarpStrength, 0.5f, 2.0f, "%.2f");
        ImGui::InputFloat("Sample A ridge min", &settings.causticsRidgeMinA, 0.005f, 0.02f, "%.3f");
        ImGui::InputFloat("Sample A ridge max", &settings.causticsRidgeMaxA, 0.005f, 0.02f, "%.3f");
        ImGui::InputFloat("Sample B ridge min", &settings.causticsRidgeMinB, 0.005f, 0.02f, "%.3f");
        ImGui::InputFloat("Sample B ridge max", &settings.causticsRidgeMaxB, 0.005f, 0.02f, "%.3f");
        ImGui::InputFloat("Focus min", &settings.causticsFocusMin, 0.01f, 0.05f, "%.3f");
        ImGui::InputFloat("Focus max", &settings.causticsFocusMax, 0.01f, 0.05f, "%.3f");
        ImGui::SliderFloat("Min surface up", &settings.causticsMinSurfaceUp, 0.0f, 1.0f, "%.2f");
    }

    if (ImGui::CollapsingHeader("Water Color", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::ColorEdit3("Shallow color", &settings.shallowWaterColor.x);
        ImGui::ColorEdit3("Mid color", &settings.midWaterColor.x);
        ImGui::ColorEdit3("Deep color", &settings.deepWaterColor.x);
        ImGui::InputFloat("Mid depth start", &settings.midWaterDepthStart, 0.1f, 1.0f, "%.2f");
        ImGui::InputFloat("Mid depth end", &settings.midWaterDepthEnd, 0.1f, 1.0f, "%.2f");
        ImGui::InputFloat("Deep depth start", &settings.deepWaterDepthStart, 0.1f, 1.0f, "%.2f");
        ImGui::InputFloat("Deep depth end", &settings.deepWaterDepthEnd, 0.1f, 1.0f, "%.2f");
    }

    ImGui::SeparatorText("Mesh");
    ImGui::TextWrapped("Water now uses one reusable mesh for all visible quadtree leaves.");
    ImGui::Text("Mesh resolution: %u x %u vertices", AppConfig::Water::kMeshVertexResolution, AppConfig::Water::kMeshVertexResolution);
    ImGui::Text("Water mesh instances: %u", context.waterMeshRenderer.instanceCount());
}

void AppPanels::drawViewportPane(Context &context)
{
    HELLO_PROFILE_SCOPE("AppPanels::DrawViewportPane");
    if (m_viewportDockId != 0)
    {
        ImGui::SetNextWindowDockID(m_viewportDockId, ImGuiCond_FirstUseEver);
    }

    ImGui::Begin("Viewport");

    const ImVec2 available = ImGui::GetContentRegionAvail();
    m_viewportPanelExtent.width = static_cast<std::uint32_t>(std::max(1.0f, available.x));
    m_viewportPanelExtent.height = static_cast<std::uint32_t>(std::max(1.0f, available.y));

    if (context.viewportTextureId != 0)
    {
        const ImVec2 imageMin = ImGui::GetCursorScreenPos();
        ImGui::Image(context.viewportTextureId,
                     ImVec2(static_cast<float>(m_viewportPanelExtent.width), static_cast<float>(m_viewportPanelExtent.height)));

        if (m_showViewportFpsCounter)
        {
            const float fps = ImGui::GetIO().Framerate;
            const std::string fpsLabel = std::to_string(static_cast<int>(std::round(fps))) + " FPS";
            const ImVec2 textPadding(8.0f, 4.0f);
            const ImVec2 textSize = ImGui::CalcTextSize(fpsLabel.c_str());
            const ImVec2 overlayMin(imageMin.x + 10.0f, imageMin.y + 10.0f);
            const ImVec2 overlayMax(overlayMin.x + textSize.x + (textPadding.x * 2.0f), overlayMin.y + textSize.y + (textPadding.y * 2.0f));

            ImDrawList *drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(overlayMin, overlayMax, IM_COL32(12, 16, 20, 185), 6.0f);
            drawList->AddText(ImVec2(overlayMin.x + textPadding.x, overlayMin.y + textPadding.y), IM_COL32(240, 248, 255, 255),
                              fpsLabel.c_str());
        }
    }

    if (m_viewportPaused)
    {
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + 12.0f, ImGui::GetWindowPos().y + 12.0f));
        ImGui::TextUnformatted("Viewport paused");
    }

    const float buttonWidth = 22.0f;
    const float buttonHeight = 56.0f;
    const float buttonX = ImGui::GetWindowPos().x + ImGui::GetStyle().FramePadding.x;
    const float buttonY = ImGui::GetWindowPos().y + (ImGui::GetWindowSize().y * 0.5f) - (buttonHeight * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(buttonX, buttonY));
    if (ImGui::Button(m_leftPaneCollapsed ? ">" : "<", ImVec2(buttonWidth, buttonHeight)))
    {
        m_leftPaneCollapsed = !m_leftPaneCollapsed;
        m_layoutDirty = true;
    }

    ImGui::End();
}
