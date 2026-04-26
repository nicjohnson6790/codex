#pragma once

#include "CameraManager.hpp"
#include "LightingSystem.hpp"
#include "PerfPanel.hpp"
#include "RenderTypes.hpp"
#include "SceneTypes.hpp"
#include "WorldGridQuadtree.hpp"

#include <imgui.h>

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

class AppPanels
{
public:
    struct Context
    {
        CameraManager& cameraManager;
        std::vector<TriangleInstance>& instances;
        const std::vector<std::string>& gpuDrivers;
        std::string_view gamepadName;
        LightingSystem& lightingSystem;
        const WorldGridQuadtree& worldGridQuadtree;
        ImTextureID viewportTextureId = 0;
    };

    void draw(Context& context);

    [[nodiscard]] Extent2D viewportExtent() const { return m_viewportPanelExtent; }
    [[nodiscard]] bool viewportPaused() const { return m_viewportPaused; }
    [[nodiscard]] bool showQuadtreeBorders() const { return m_showQuadtreeBorders; }

private:
    void drawDockSpace();
    void applyDockLayout();
    void drawInfoPane(Context& context);
    void drawControlsTab(Context& context);
    void drawDebugTab(Context& context);
    void drawViewportPane(Context& context);

    Extent2D m_viewportPanelExtent{ 640, 480 };
    bool m_dockLayoutInitialized = false;
    bool m_leftPaneCollapsed = false;
    bool m_viewportPaused = false;
    bool m_showQuadtreeBorders = true;
    bool m_layoutDirty = false;
    ImGuiID m_viewportDockId = 0;
    PerfPanel m_perfPanel;
};
