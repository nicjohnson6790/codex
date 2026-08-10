#pragma once

#include "Gameplay.hpp"
#include "LightingSystem.hpp"
#include "Multiplayer/IMultiplayerTransport.hpp"
#include "Multiplayer/MultiplayerProtocol.hpp"
#include "Position.hpp"
#include "platform/SteamService.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>

class MultiplayerManager
{
public:
    enum class SessionState
    {
        Offline,
        Hosting,
        Joining,
        InLobby,
        Leaving,
        Error,
    };

    struct RemoteEntity
    {
        std::uint64_t steamId = 0;
        std::string personaName;
        Position drawPosition;
        float yawRadians = 0.0f;
        glm::vec3 color{ 0.2f, 0.8f, 1.0f };
        float packetAgeSeconds = 0.0f;
    };

    struct DebugSnapshot
    {
        SessionState state = SessionState::Offline;
        std::uint64_t lobbyId = 0;
        std::uint64_t hostId = 0;
        std::size_t connectionCount = 0;
        std::size_t remoteEntityCount = 0;
        float newestPacketAgeSeconds = 0.0f;
        std::uint64_t sentPlayerStateCount = 0;
        std::uint64_t receivedPlayerStateCount = 0;
        float timeSyncDriftHours = 0.0f;
        std::uint32_t timeRevision = 0;
        std::string connectionState;
        std::string error;
    };

    void update(
        SteamService& steamService,
        IMultiplayerTransport& transport,
        PlayerPawn& localPlayer,
        LightingSystem& lightingSystem,
        float deltaTimeSeconds,
        double nowSeconds);
    void hostLobby(SteamService& steamService, IMultiplayerTransport& transport);
    void joinLobby(SteamService& steamService, IMultiplayerTransport& transport, std::uint64_t lobbyId);
    void leaveSession(SteamService& steamService, IMultiplayerTransport& transport);

    [[nodiscard]] SessionState state() const { return m_state; }
    [[nodiscard]] bool isHost() const { return m_state == SessionState::Hosting; }
    [[nodiscard]] const std::vector<RemoteEntity>& remoteEntities() const { return m_remoteEntities; }
    [[nodiscard]] DebugSnapshot debugSnapshot() const { return m_debugSnapshot; }
    [[nodiscard]] static const char* stateName(SessionState state);

private:
    struct RemotePlayerState
    {
        std::uint64_t steamId = 0;
        std::string personaName;
        Position lastPosition;
        glm::dvec3 velocity{ 0.0 };
        double yawRadians = 0.0;
        double lastReceiveTimeSeconds = 0.0;
        Position smoothedDrawPosition;
        bool hasDrawPosition = false;
    };

    void handleTransportEvent(
        const IMultiplayerTransport::Event& event,
        SteamService& steamService,
        IMultiplayerTransport& transport,
        LightingSystem& lightingSystem,
        double nowSeconds,
        float deltaTimeSeconds);
    void handleMessage(
        std::uint64_t peerId,
        const std::vector<std::uint8_t>& payload,
        SteamService& steamService,
        IMultiplayerTransport& transport,
        LightingSystem& lightingSystem,
        double nowSeconds,
        float deltaTimeSeconds);
    void sendHello(IMultiplayerTransport& transport, std::uint64_t peerId, SteamService& steamService, double nowSeconds);
    void sendLocalPlayerState(
        IMultiplayerTransport& transport,
        const PlayerPawn& localPlayer,
        SteamService& steamService,
        double nowSeconds);
    void sendHostTimeSync(IMultiplayerTransport& transport, const LightingSystem& lightingSystem, double nowSeconds, bool reliable);
    void applyTimeSync(const MultiplayerProtocol::TimeSync& sync, LightingSystem& lightingSystem, float deltaTimeSeconds);
    void rebuildRemoteEntities(double nowSeconds, float deltaTimeSeconds);
    void refreshDebugSnapshot(SteamService& steamService, IMultiplayerTransport& transport, double nowSeconds);
    void refreshLobbyDrivenState(SteamService& steamService, IMultiplayerTransport& transport, double nowSeconds);
    void broadcastReliable(IMultiplayerTransport& transport, const void* data, std::size_t size, std::uint64_t exceptPeer = 0);
    void broadcastUnreliable(IMultiplayerTransport& transport, const void* data, std::size_t size, std::uint64_t exceptPeer = 0);
    [[nodiscard]] std::uint32_t nextSequence() { return ++m_sequence; }
    [[nodiscard]] glm::vec3 colorForSteamId(std::uint64_t steamId) const;

    SessionState m_state = SessionState::Offline;
    std::unordered_map<std::uint64_t, RemotePlayerState> m_remotePlayers;
    std::vector<RemoteEntity> m_remoteEntities;
    DebugSnapshot m_debugSnapshot;
    std::uint32_t m_sequence = 0;
    std::uint32_t m_timeRevision = 0;
    double m_lastPlayerSendSeconds = -100.0;
    double m_lastTimeSyncSeconds = -100.0;
    double m_lastHostConnectAttemptSeconds = -100.0;
    std::uint64_t m_sentPlayerStateCount = 0;
    std::uint64_t m_receivedPlayerStateCount = 0;
    float m_timeSyncDriftHours = 0.0f;
    std::string m_error;
};
