#pragma once

#include "CameraManager.hpp"
#include "FoliageCanopyRenderer.hpp"
#include "Gameplay.hpp"
#include "LightingSystem.hpp"
#include "Multiplayer/MultiplayerManager.hpp"
#include "NearbyFoliageRenderer.hpp"
#include "PerfPanel.hpp"
#include "platform/SteamService.hpp"
#include "RenderTypes.hpp"
#include "SceneTypes.hpp"
#include "SkyboxRenderer.hpp"
#include "SDLRenderer.hpp"
#include "FoliageImposterRenderer.hpp"
#include "QuadtreeWaterMeshRenderer.hpp"
#include "WorldGridFoliageCanopyManager.hpp"
#include "WorldGridFoliageManager.hpp"
#include "WorldGridNearbyFoliageManager.hpp"
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
    enum class MultiplayerCommand
    {
        None,
        CreateLobby,
        JoinLobby,
        LeaveLobby,
    };

    struct Context
    {
        CameraManager& cameraManager;
        SDLRenderer& renderer;
        const SteamService& steamService;
        MultiplayerManager& multiplayerManager;
        PlayerPawn& playerPawn;
        CollisionManager& collisionManager;
        bool& playerFollowCameraEnabled;
        const std::vector<std::string>& gpuDrivers;
        std::string_view gamepadName;
        LightingSystem& lightingSystem;
        SkyboxRenderer& skyboxRenderer;
        FoliageCanopyRenderer& foliageCanopyRenderer;
        WorldGridFoliageCanopyManager& foliageCanopyManager;
        FoliageImposterRenderer& foliageRenderer;
        NearbyFoliageRenderer& nearbyFoliageRenderer;
        WorldGridNearbyFoliageManager& nearbyFoliageManager;
        WorldGridFoliageManager& foliageManager;
        QuadtreeWaterMeshRenderer& waterMeshRenderer;
        WorldGridQuadtreeWaterManager& waterManager;
        WorldGridQuadtree& worldGridQuadtree;
        ImTextureID viewportTextureId = 0;
    };

    void draw(Context& context);

    [[nodiscard]] Extent2D viewportExtent() const { return m_viewportPanelExtent; }
    [[nodiscard]] bool viewportPaused() const { return m_viewportPaused; }
    [[nodiscard]] bool showQuadtreeBorders() const { return m_showQuadtreeBorders; }
    [[nodiscard]] MultiplayerCommand consumeMultiplayerCommand(std::uint64_t& lobbyId)
    {
        const MultiplayerCommand command = m_pendingMultiplayerCommand;
        lobbyId = m_pendingLobbyId;
        m_pendingMultiplayerCommand = MultiplayerCommand::None;
        m_pendingLobbyId = 0;
        return command;
    }

private:
    void drawDockSpace();
    void applyDockLayout();
    void drawInfoPane(Context& context);
    void drawControlsTab(Context& context);
    void drawSteamTab(Context& context);
    void drawMultiplayerTab(Context& context);
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
    MultiplayerCommand m_pendingMultiplayerCommand = MultiplayerCommand::None;
    std::uint64_t m_pendingLobbyId = 0;
    ImGuiID m_viewportDockId = 0;
    PerfPanel m_perfPanel;
};
