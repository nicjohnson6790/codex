#pragma once

#include "CameraManager.hpp"
#include "LightingSystem.hpp"
#include "PerfPanel.hpp"
#include "RenderTypes.hpp"
#include "SceneTypes.hpp"
#include "SkyboxRenderer.hpp"
#include "SDLRenderer.hpp"
#include "QuadtreeWaterMeshRenderer.hpp"
#include "WorldGridQuadtreeWaterManager.hpp"
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
        SDLRenderer& renderer;
        std::vector<TriangleInstance>& instances;
        const std::vector<std::string>& gpuDrivers;
        std::string_view gamepadName;
        LightingSystem& lightingSystem;
        SkyboxRenderer& skyboxRenderer;
        QuadtreeWaterMeshRenderer& waterMeshRenderer;
        WorldGridQuadtreeWaterManager& waterManager;
        WorldGridQuadtree& worldGridQuadtree;
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
    void drawTerrainTab(Context& context);
    void drawWaterTab(Context& context);
    void drawDebugTab(Context& context);
    void drawViewportPane(Context& context);

    Extent2D m_viewportPanelExtent{ 1280, 800 };
    bool m_dockLayoutInitialized = false;
    bool m_leftPaneCollapsed = false;
    bool m_viewportPaused = false;
    bool m_showViewportFpsCounter = true;
    bool m_showQuadtreeBorders = false;
    bool m_layoutDirty = false;
    ImGuiID m_viewportDockId = 0;
    PerfPanel m_perfPanel;
};
