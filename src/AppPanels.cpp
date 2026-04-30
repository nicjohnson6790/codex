#include "AppPanels.hpp"

#include "AppConfig.hpp"
#include "PerformanceCapture.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <string>

void AppPanels::draw(Context& context)
{
    HELLO_PROFILE_SCOPE_GROUPS("AppPanels::Draw", ProfileScopeGroup::ImGui);
    drawDockSpace();
    drawInfoPane(context);
    drawViewportPane(context);
}

void AppPanels::drawDockSpace()
{
    HELLO_PROFILE_SCOPE("AppPanels::DrawDockSpace");
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground;

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

    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
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

void AppPanels::drawInfoPane(Context& context)
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

void AppPanels::drawControlsTab(Context& context)
{
    HELLO_PROFILE_SCOPE("AppPanels::DrawControlsTab");
    ImGui::TextUnformatted("Stack");
    ImGui::Separator();
    ImGui::Text("SDL3 window + SDL GPU renderer + Dear ImGui");
    ImGui::Text("Viewport texture size: %u x %u", m_viewportPanelExtent.width, m_viewportPanelExtent.height);
    ImGui::Text("Gamepad: %.*s",
        static_cast<int>(context.gamepadName.size()),
        context.gamepadName.data());

    ImGui::Spacing();
    ImGui::TextUnformatted("SDL GPU Drivers");
    ImGui::Separator();
    for (const std::string& driver : context.gpuDrivers)
    {
        ImGui::BulletText("%s", driver.c_str());
    }

    const CameraManager::Camera& activeCamera = context.cameraManager.activeCamera();
    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("Active: %s", activeCamera.name.c_str());
        ImGui::Text(
            "Camera Cell: (%lld, %lld)",
            static_cast<long long>(activeCamera.position.gridX()),
            static_cast<long long>(activeCamera.position.gridY())
        );
        const glm::dvec3& cameraLocal = activeCamera.position.localPosition();
        ImGui::Text("Camera Local: (%.3f, %.3f, %.3f)", cameraLocal.x, cameraLocal.y, cameraLocal.z);
        if (ImGui::Button("Add Camera"))
        {
            const std::size_t newCameraIndex = context.cameraManager.cameraCount() + 1;
            context.cameraManager.createCamera(
                "Camera " + std::to_string(newCameraIndex),
                activeCamera.position,
                activeCamera.forward,
                activeCamera.up
            );
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

    if (ImGui::CollapsingHeader("Triangle Instances", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::Button("Add Triangle"))
        {
            context.instances.push_back({ .position = Position(0, 0, { 0.0, 0.0, 0.0 }) });
        }

        ImGui::SameLine();
        if (ImGui::Button("Reset Layout"))
        {
            context.instances = {
                { .position = Position(-1, 0, { static_cast<double>(Position::kCellSize) - 0.25, 0.0, 0.0 }) },
                { .position = Position(0, 0, { 0.25, 0.0, 0.0 }) },
            };
        }

        for (std::size_t index = 0; index < context.instances.size(); ++index)
        {
            TriangleInstance& instance = context.instances[index];
            ImGui::PushID(static_cast<int>(index));
            ImGui::SeparatorText(("Triangle " + std::to_string(index)).c_str());
            long long gridX = static_cast<long long>(instance.position.gridX());
            long long gridY = static_cast<long long>(instance.position.gridY());
            glm::dvec3 localPosition = instance.position.localPosition();
            ImGui::InputScalar("Grid X", ImGuiDataType_S64, &gridX);
            ImGui::InputScalar("Grid Y", ImGuiDataType_S64, &gridY);
            ImGui::InputDouble("Local X", &localPosition.x, 0.1, 1.0, "%.3f");
            ImGui::InputDouble("Local Y", &localPosition.y, 0.1, 1.0, "%.3f");
            ImGui::InputDouble("Local Z", &localPosition.z, 0.1, 1.0, "%.3f");
            instance.position.setGridX(static_cast<std::int64_t>(gridX));
            instance.position.setGridY(static_cast<std::int64_t>(gridY));
            instance.position.setLocalPosition(localPosition);
            if (ImGui::Button("Remove") && context.instances.size() > 1)
            {
                context.instances.erase(context.instances.begin() + static_cast<std::ptrdiff_t>(index));
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }

    if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen))
    {
        LightingSystem::SunLight& sun = context.lightingSystem.sun();
        ImGui::SliderFloat("Azimuth", &sun.azimuthDegrees, -180.0f, 180.0f, "%.1f deg");
        ImGui::SliderFloat("Elevation", &sun.elevationDegrees, 1.0f, 89.0f, "%.1f deg");
        ImGui::SliderFloat("Time of day", &sun.timeOfDayHours, 0.0f, 24.0f, "%.2f h");
        ImGui::InputFloat("Day length (s)", &sun.dayLengthSeconds, 1.0f, 10.0f, "%.1f");
        sun.dayLengthSeconds = std::max(sun.dayLengthSeconds, 0.1f);
        ImGui::InputFloat("Time factor", &sun.timeFactor, 0.1f, 1.0f, "%.2f");
        ImGui::ColorEdit3("Color", &sun.color.x);
        ImGui::SliderFloat("Intensity", &sun.intensity, 0.0f, 4.0f, "%.2f");
    }

    if (ImGui::CollapsingHeader("Atmosphere", ImGuiTreeNodeFlags_DefaultOpen))
    {
        SkyboxRenderer::AtmosphereSettings& atmosphere = context.skyboxRenderer.atmosphereSettings();

        ImGui::InputFloat("Atmosphere height (m)", &atmosphere.atmosphereHeight, 1000.0f, 10000.0f, "%.0f");
        ImGui::InputFloat("Distance range (m)", &atmosphere.atmosphereDistanceRange, 10000.0f, 100000.0f, "%.0f");
        ImGui::InputFloat("Exposure", &atmosphere.exposure, 0.1f, 0.5f, "%.2f");
        ImGui::InputFloat("Alpha scale", &atmosphere.alphaScale, 0.05f, 0.2f, "%.2f");

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

        if (ImGui::TreeNode("Ambient And Haze"))
        {
            ImGui::InputFloat("Ambient sky scale", &atmosphere.ambientSkyScale, 0.05f, 0.2f, "%.2f");
            ImGui::InputFloat("Ambient blue bias", &atmosphere.ambientBlueBias, 0.01f, 0.05f, "%.2f");
            ImGui::InputFloat("Ambient solar infl.", &atmosphere.ambientSolarInfluence, 0.01f, 0.05f, "%.2f");
            ImGui::InputFloat("Ambient twilight infl.", &atmosphere.ambientTwilightInfluence, 0.01f, 0.05f, "%.2f");
            ImGui::ColorEdit3("Ambient blue tint", &atmosphere.ambientBlueTintR);
            ImGui::InputFloat("Rayleigh tint scale", &atmosphere.rayleighTintScale, 0.05f, 0.2f, "%.2f");
            ImGui::ColorEdit3("Haze color", &atmosphere.hazeColorR);
            ImGui::InputFloat("Haze strength", &atmosphere.hazeStrength, 0.01f, 0.05f, "%.2f");
            ImGui::InputFloat("Path fog distance", &atmosphere.pathFogDistance, 1000.0f, 10000.0f, "%.0f");
            ImGui::InputFloat("Long haze distance", &atmosphere.longRangeHazeDistance, 1000.0f, 10000.0f, "%.0f");
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Sun Shape And Sunset"))
        {
            ImGui::InputFloat("Aureole power", &atmosphere.aureolePower, 1.0f, 8.0f, "%.0f");
            ImGui::InputFloat("Aureole strength", &atmosphere.aureoleStrength, 0.05f, 0.2f, "%.2f");
            ImGui::InputFloat("Sun disk power", &atmosphere.sunDiskPower, 16.0f, 128.0f, "%.0f");
            ImGui::InputFloat("Sun disk strength", &atmosphere.sunDiskStrength, 0.5f, 2.0f, "%.2f");
            ImGui::InputFloat("Sun glow power", &atmosphere.sunGlowPower, 1.0f, 4.0f, "%.0f");
            ImGui::ColorEdit3("Sunset tint", &atmosphere.sunsetTintR);
            ImGui::InputFloat("Sunset strength", &atmosphere.sunsetStrength, 0.01f, 0.05f, "%.2f");
            ImGui::InputFloat("Sunset sunward boost", &atmosphere.sunsetSunwardBoost, 0.05f, 0.2f, "%.2f");
            ImGui::InputFloat("Sunset dist min", &atmosphere.sunsetDistanceMin, 0.01f, 0.05f, "%.2f");
            ImGui::InputFloat("Sunset dist max", &atmosphere.sunsetDistanceMax, 0.05f, 0.2f, "%.2f");
            ImGui::TreePop();
        }

        context.skyboxRenderer.sanitizeAtmosphereSettings();

        if (ImGui::Button("Reset Atmosphere Defaults"))
        {
            context.skyboxRenderer.resetAtmosphereSettings();
        }
        ImGui::SameLine();
        if (ImGui::Button("Regenerate + Upload LUT"))
        {
            context.skyboxRenderer.regenerateAtmosphereLut();
        }
    }
}

void AppPanels::drawTerrainTab(Context& context)
{
    HELLO_PROFILE_SCOPE("AppPanels::DrawTerrainTab");

    TerrainNoiseSettings& settings = context.worldGridQuadtree.terrainSettings();
    int computeDispatchBudget = static_cast<int>(context.worldGridQuadtree.computeDispatchBudget());

    ImGui::TextWrapped("Terrain slices keep their generated heightmaps in the LRU cache. After changing these values, regenerate the cache to rebuild terrain with the new noise settings.");
    ImGui::Spacing();

    ImGui::InputDouble("Base height", &settings.baseHeight, 100.0, 500.0, "%.1f");
    ImGui::TextWrapped("Blend channel steers which terrain family wins: low values favor hills, mid values favor the 12k layer, and high values favor the 4k layer.");
    ImGui::Spacing();

    auto drawFractalLayerEditor = [](const char* label, TerrainFractalNoiseLayerSettings& layer)
    {
        int octaveCount = static_cast<int>(layer.octaveCount);
        if (!ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }

        ImGui::InputDouble("Wavelength", &layer.wavelength, 100.0, 500.0, "%.1f");
        ImGui::InputDouble("Amplitude", &layer.amplitude, 100.0, 500.0, "%.1f");
        ImGui::InputDouble("Height bias", &layer.bias, 100.0, 500.0, "%.1f");
        ImGui::InputDouble("Initial frequency", &layer.initialFrequency, 0.05, 0.25, "%.3f");
        ImGui::InputDouble("Initial amplitude", &layer.initialAmplitude, 0.05, 0.25, "%.3f");
        ImGui::SliderInt("Octaves", &octaveCount, 1, 12);
        layer.octaveCount = static_cast<std::uint32_t>(std::max(octaveCount, 1));
        ImGui::InputDouble("Octave frequency scale", &layer.octaveFrequencyScale, 0.05, 0.25, "%.2f");
        ImGui::InputDouble("Octave amplitude scale", &layer.octaveAmplitudeScale, 0.02, 0.10, "%.2f");
        ImGui::InputDouble("Gradient dampening k", &layer.gradientDampenStrength, 0.05, 0.25, "%.2f");
        ImGui::InputDouble("Octave rotation deg", &layer.octaveRotationDegrees, 1.0, 5.0, "%.1f");
    };

    if (ImGui::CollapsingHeader("Blend Channel", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int blendOctaveCount = static_cast<int>(settings.blend.octaveCount);
        ImGui::InputDouble("Blend wavelength", &settings.blend.wavelength, 100.0, 500.0, "%.1f");
        ImGui::InputDouble("Blend initial frequency", &settings.blend.initialFrequency, 0.05, 0.25, "%.3f");
        ImGui::InputDouble("Blend initial amplitude", &settings.blend.initialAmplitude, 0.05, 0.25, "%.3f");
        ImGui::SliderInt("Blend octaves", &blendOctaveCount, 1, 12);
        settings.blend.octaveCount = static_cast<std::uint32_t>(std::max(blendOctaveCount, 1));
        ImGui::InputDouble("Blend octave frequency scale", &settings.blend.octaveFrequencyScale, 0.05, 0.25, "%.2f");
        ImGui::InputDouble("Blend octave amplitude scale", &settings.blend.octaveAmplitudeScale, 0.02, 0.10, "%.2f");
        ImGui::InputDouble("Blend gradient dampening k", &settings.blend.gradientDampenStrength, 0.05, 0.25, "%.2f");
        ImGui::InputDouble("Blend octave rotation deg", &settings.blend.octaveRotationDegrees, 1.0, 5.0, "%.1f");
        ImGui::InputDouble("Low threshold", &settings.blend.lowThreshold, 0.01, 0.05, "%.2f");
        ImGui::InputDouble("High threshold", &settings.blend.highThreshold, 0.01, 0.05, "%.2f");
        ImGui::InputDouble("Low transition width", &settings.blend.lowTransitionWidth, 0.01, 0.05, "%.2f");
        ImGui::InputDouble("High transition width", &settings.blend.highTransitionWidth, 0.01, 0.05, "%.2f");
    }

    ImGui::PushID("HillsLayer");
    drawFractalLayerEditor("Rolling Hills", settings.hills);
    ImGui::PopID();

    ImGui::PushID("MediumLayer");
    drawFractalLayerEditor("12k Layer", settings.mediumDetail);
    ImGui::PopID();

    ImGui::PushID("HighLayer");
    drawFractalLayerEditor("4k Layer", settings.highDetail);
    ImGui::PopID();

    ImGui::SliderInt("Compute dispatches/frame", &computeDispatchBudget, 1, 64);
    context.worldGridQuadtree.setComputeDispatchBudget(static_cast<std::uint16_t>(std::max(computeDispatchBudget, 1)));

    settings = sanitizeTerrainNoiseSettings(settings);

    ImGui::Spacing();
    if (ImGui::Button("Regenerate Terrain Cache"))
    {
        context.worldGridQuadtree.clearTerrainCache();
    }

    ImGui::Spacing();
    ImGui::Text("Resident slices: %u", context.worldGridQuadtree.residentCount());
    ImGui::Text("Queued leaves: %u", context.worldGridQuadtree.queuedCount());
}

void AppPanels::drawDebugTab(Context& context)
{
    HELLO_PROFILE_SCOPE("AppPanels::DrawDebugTab");
    const WorldGridQuadtree::TreeData& treeData = context.worldGridQuadtree.treeData;

    ImGui::TextUnformatted("Quadtree");
    ImGui::Separator();
    ImGui::Checkbox("Show quadtree borders", &m_showQuadtreeBorders);
    ImGui::Text("Generated cells: 9");
    ImGui::Text("Drawable nodes: %u", treeData.drawableNodeCount);
    ImGui::Text("Started subdividing this frame: %u", treeData.subdivisionCountThisFrame);
    ImGui::Text("Started collapsing this frame: %u", treeData.collapseCountThisFrame);
    ImGui::Text("Max depth: %u", treeData.maxDepth);
    ImGui::Text("Minimum quad size: %.0f", AppConfig::Quadtree::kMinimumQuadSize);
    ImGui::Spacing();
    ImGui::TextUnformatted("Heightmap Manager");
    ImGui::Separator();
    ImGui::Text("Resident slices: %u", context.worldGridQuadtree.residentCount());
    ImGui::Text("Queued leaves: %u", context.worldGridQuadtree.queuedCount());
    ImGui::Spacing();
    ImGui::TextUnformatted("Water");
    ImGui::Separator();
    ImGui::Text("Queued water instances: %u", context.waterManager.queuedCount());
    ImGui::Text("Water mesh instances: %u", context.waterMeshRenderer.instanceCount());
    ImGui::Text(
        "Water mesh resolution: %u x %u vertices",
        AppConfig::Water::kMeshVertexResolution,
        AppConfig::Water::kMeshVertexResolution);
    ImGui::Spacing();
    ImGui::TextWrapped("The quadtree is rebuilt around the active camera and includes the current grid cell plus the eight surrounding cells.");
}

void AppPanels::drawWaterTab(Context& context)
{
    HELLO_PROFILE_SCOPE("AppPanels::DrawWaterTab");

    WaterSettings& settings = context.waterManager.settings();
    ImGui::Checkbox("Enabled", &settings.enabled);
    ImGui::Checkbox("Show LOD tint", &settings.showLodTint);
    ImGui::InputFloat("Water level", &settings.waterLevel, 1.0f, 10.0f, "%.2f");
    ImGui::InputFloat("Global amplitude", &settings.globalAmplitude, 0.05f, 0.25f, "%.2f");
    ImGui::InputFloat("Global choppiness", &settings.globalChoppiness, 0.05f, 0.25f, "%.2f");
    ImGui::InputFloat("Depth (m)", &settings.depthMeters, 1.0f, 10.0f, "%.2f");
    ImGui::InputFloat("Low cutoff", &settings.lowCutoff, 0.0001f, 0.001f, "%.4f");
    ImGui::InputFloat("High cutoff", &settings.highCutoff, 0.1f, 1.0f, "%.2f");
    ImGui::InputFloat(
        "Dry terrain cutoff above water (m)",
        &settings.maxTerrainMinHeightAboveWaterToDraw,
        1.0f,
        10.0f,
        "%.2f");

    int cascadeCount = static_cast<int>(settings.cascadeCount);
    ImGui::SliderInt("Cascade count", &cascadeCount, 0, static_cast<int>(AppConfig::Water::kMaxCascadeCount));
    settings.cascadeCount = static_cast<std::uint32_t>(std::max(cascadeCount, 0));

    if (ImGui::CollapsingHeader("Water Cascades", ImGuiTreeNodeFlags_DefaultOpen))
    {
        for (std::uint32_t index = 0; index < settings.cascadeCount; ++index)
        {
            WaterCascadeSettings& cascade = settings.cascades[index];
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
            int updateModulo = static_cast<int>(cascade.updateModulo);
            ImGui::SliderInt("Update modulo", &updateModulo, 1, 8);
            cascade.updateModulo = static_cast<std::uint32_t>(std::max(updateModulo, 1));
            ImGui::PopID();
        }
    }

    ImGui::SeparatorText("Mesh");
    ImGui::TextWrapped("Water now uses one reusable mesh for all visible quadtree leaves.");
    ImGui::Text(
        "Mesh resolution: %u x %u vertices",
        AppConfig::Water::kMeshVertexResolution,
        AppConfig::Water::kMeshVertexResolution);
    ImGui::Text("Active instances: %u", context.waterMeshRenderer.instanceCount());
}

void AppPanels::drawViewportPane(Context& context)
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
        ImGui::Image(
            context.viewportTextureId,
            ImVec2(static_cast<float>(m_viewportPanelExtent.width), static_cast<float>(m_viewportPanelExtent.height))
        );
    }

    if (m_viewportPaused)
    {
        ImGui::SetCursorScreenPos(ImVec2(
            ImGui::GetWindowPos().x + 12.0f,
            ImGui::GetWindowPos().y + 12.0f
        ));
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
