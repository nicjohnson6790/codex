#include "AppPanels.hpp"

#include "AppConfig.hpp"
#include "PerformanceCapture.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <string>

void AppPanels::draw(Context& context)
{
    HELLO_PROFILE_SCOPE("AppPanels::Draw");
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

    ImGui::Spacing();
    ImGui::TextUnformatted("SDL GPU Drivers");
    ImGui::Separator();
    for (const std::string& driver : context.gpuDrivers)
    {
        ImGui::BulletText("%s", driver.c_str());
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
        ImGui::ColorEdit3("Color", &sun.color.x);
        ImGui::SliderFloat("Intensity", &sun.intensity, 0.0f, 4.0f, "%.2f");
    }
}

void AppPanels::drawDebugTab(Context& context)
{
    HELLO_PROFILE_SCOPE("AppPanels::DrawDebugTab");
    const WorldGridQuadtree::TreeData& treeData = context.worldGridQuadtree.treeData;

    ImGui::TextUnformatted("Quadtree");
    ImGui::Separator();
    ImGui::Checkbox("Show quadtree borders", &m_showQuadtreeBorders);
    ImGui::Text("Generated cells: 9");
    ImGui::Text("Visible nodes: %u", treeData.visibleNodeCount);
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
    ImGui::TextWrapped("The quadtree is rebuilt around the active camera and includes the current grid cell plus the eight surrounding cells.");
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
