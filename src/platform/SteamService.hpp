#pragma once

#include "GamepadInput.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
#include <steam/steam_api.h>
#endif

class SteamService
{
public:
    struct LobbyMember
    {
        std::uint64_t steamId = 0;
        std::string personaName;
    };

    struct FriendLobby
    {
        std::uint64_t friendSteamId = 0;
        std::uint64_t lobbyId = 0;
        std::string personaName;
    };

    void initialize(bool requested, const std::filesystem::path& inputManifestPath);
    void pumpCallbacks();
    void shutdown();

    [[nodiscard]] bool compiledIn() const;
    [[nodiscard]] bool requested() const { return m_requested; }
    [[nodiscard]] bool initialized() const { return m_initialized; }
    [[nodiscard]] std::uint32_t appId() const { return m_appId; }
    [[nodiscard]] std::uint64_t userId() const { return m_userId; }
    [[nodiscard]] const std::string& personaName() const { return m_personaName; }
    [[nodiscard]] bool steamInputInitialized() const { return m_steamInputInitialized; }
    [[nodiscard]] bool steamInputManifestLoaded() const { return m_steamInputManifestLoaded; }
    [[nodiscard]] int steamInputControllerCount() const { return m_steamInputControllerCount; }
    [[nodiscard]] std::uint64_t activeInputHandle() const { return m_activeInputHandle; }
    [[nodiscard]] bool steamInputActionsReady() const;
    [[nodiscard]] bool lastSteamInputStateActive() const { return m_lastSteamInputStateActive; }
    [[nodiscard]] float lastMoveX() const { return m_lastMoveX; }
    [[nodiscard]] float lastMoveY() const { return m_lastMoveY; }
    [[nodiscard]] float lastLookX() const { return m_lastLookX; }
    [[nodiscard]] float lastLookY() const { return m_lastLookY; }
    [[nodiscard]] GamepadState pollGamepadState();
    [[nodiscard]] bool lobbyRequestPending() const { return m_lobbyRequestPending; }
    [[nodiscard]] std::uint64_t currentLobbyId() const { return m_currentLobbyId; }
    [[nodiscard]] std::uint64_t currentLobbyOwnerId() const { return m_currentLobbyOwnerId; }
    [[nodiscard]] const std::vector<LobbyMember>& lobbyMembers() const { return m_lobbyMembers; }
    [[nodiscard]] std::vector<FriendLobby> joinableFriendLobbies() const;
    [[nodiscard]] std::string personaNameFor(std::uint64_t steamId) const;
    [[nodiscard]] bool createLobby(int maxMembers = 4);
    [[nodiscard]] bool joinLobby(std::uint64_t lobbyId);
    void leaveLobby();

private:
    void initializeSteamInput(const std::filesystem::path& inputManifestPath);
    void refreshSteamInputControllers();
    void refreshLobbyMembers();
    void clearLobbyState();
    [[nodiscard]] bool digitalAction(std::uint64_t inputHandle, std::uint64_t actionHandle) const;

#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    STEAM_CALLBACK(SteamService, onSteamInputDeviceConnected, SteamInputDeviceConnected_t);
    STEAM_CALLBACK(SteamService, onSteamInputDeviceDisconnected, SteamInputDeviceDisconnected_t);
    STEAM_CALLBACK(SteamService, onLobbyChatUpdate, LobbyChatUpdate_t);
    STEAM_CALLBACK(SteamService, onGameLobbyJoinRequested, GameLobbyJoinRequested_t);
    void onLobbyCreated(LobbyCreated_t* event, bool ioFailure);
    void onLobbyEntered(LobbyEnter_t* event, bool ioFailure);
    CCallResult<SteamService, LobbyCreated_t> m_lobbyCreatedCallResult;
    CCallResult<SteamService, LobbyEnter_t> m_lobbyEnteredCallResult;
#endif

    bool m_requested = false;
    bool m_initialized = false;
    bool m_steamInputInitialized = false;
    bool m_steamInputManifestLoaded = false;
    std::uint32_t m_appId = 0;
    std::uint64_t m_userId = 0;
    int m_steamInputControllerCount = 0;
    std::uint64_t m_activeInputHandle = 0;
    std::uint64_t m_terrainActionSet = 0;
    std::uint64_t m_moveAction = 0;
    std::uint64_t m_lookAction = 0;
    std::uint64_t m_ascendAction = 0;
    std::uint64_t m_descendAction = 0;
    std::uint64_t m_rollLeftAction = 0;
    std::uint64_t m_rollRightAction = 0;
    std::uint64_t m_sprintAction = 0;
    std::uint64_t m_alignUpAction = 0;
    bool m_lastSteamInputStateActive = false;
    bool m_lobbyRequestPending = false;
    float m_lastMoveX = 0.0f;
    float m_lastMoveY = 0.0f;
    float m_lastLookX = 0.0f;
    float m_lastLookY = 0.0f;
    std::uint64_t m_currentLobbyId = 0;
    std::uint64_t m_currentLobbyOwnerId = 0;
    std::vector<LobbyMember> m_lobbyMembers;
    std::string m_personaName;
};
