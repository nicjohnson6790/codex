#include "Multiplayer/MultiplayerManager.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include <glm/geometric.hpp>

namespace
{
constexpr double kPlayerSendIntervalSeconds = 1.0 / 15.0;
constexpr double kHostTimeSyncIntervalSeconds = 3.0;
constexpr double kHostConnectRetrySeconds = 2.0;
constexpr double kPredictionClampSeconds = 0.35;
constexpr double kPredictionSnapMeters = 20.0;
constexpr double kPositionSmoothRate = 12.0;
constexpr float kTimeSnapDriftHours = 0.15f;
constexpr float kTimeSmoothRate = 0.08f;

MultiplayerProtocol::NetworkPosition toNetworkPosition(const Position& position)
{
    const glm::dvec3& local = position.localPosition();
    return {
        .worldX = position.gridX(),
        .worldY = position.gridY(),
        .localX = static_cast<float>(local.x),
        .localY = static_cast<float>(local.y),
        .localZ = static_cast<float>(local.z),
    };
}

Position fromNetworkPosition(const MultiplayerProtocol::NetworkPosition& position)
{
    return Position(
        position.worldX,
        position.worldY,
        { position.localX, position.localY, position.localZ });
}

template <typename Message>
bool readMessage(const std::vector<std::uint8_t>& payload, Message& message)
{
    if (payload.size() != sizeof(Message))
    {
        return false;
    }
    std::memcpy(&message, payload.data(), sizeof(Message));
    return message.header.magic == MultiplayerProtocol::kMagic &&
        message.header.version == MultiplayerProtocol::kVersion;
}

void copyPersona(char (&destination)[64], const std::string& source)
{
    std::memset(destination, 0, sizeof(destination));
    std::memcpy(destination, source.c_str(), std::min(source.size(), sizeof(destination) - 1));
}

float wrapHourDelta(float from, float to)
{
    float delta = std::fmod(to - from, 24.0f);
    if (delta > 12.0f)
    {
        delta -= 24.0f;
    }
    else if (delta < -12.0f)
    {
        delta += 24.0f;
    }
    return delta;
}

float wrapHour(float value)
{
    value = std::fmod(value, 24.0f);
    return value < 0.0f ? value + 24.0f : value;
}
}

void MultiplayerManager::update(
    SteamService& steamService,
    IMultiplayerTransport& transport,
    PlayerPawn& localPlayer,
    LightingSystem& lightingSystem,
    float deltaTimeSeconds,
    double nowSeconds)
{
    refreshLobbyDrivenState(steamService, transport, nowSeconds);

    std::vector<IMultiplayerTransport::Event> events;
    transport.pollEvents(events);
    for (const IMultiplayerTransport::Event& event : events)
    {
        handleTransportEvent(event, steamService, transport, lightingSystem, nowSeconds, deltaTimeSeconds);
    }

    if (m_state == SessionState::Hosting || m_state == SessionState::InLobby)
    {
        sendLocalPlayerState(transport, localPlayer, steamService, nowSeconds);
    }
    if (m_state == SessionState::Hosting && nowSeconds - m_lastTimeSyncSeconds >= kHostTimeSyncIntervalSeconds)
    {
        sendHostTimeSync(transport, lightingSystem, nowSeconds, true);
    }

    rebuildRemoteEntities(nowSeconds, deltaTimeSeconds);
    refreshDebugSnapshot(steamService, transport, nowSeconds);
}

void MultiplayerManager::hostLobby(SteamService& steamService, IMultiplayerTransport& transport)
{
    m_error.clear();
    if (m_state != SessionState::Offline && m_state != SessionState::Error)
    {
        leaveSession(steamService, transport);
    }
    if (!steamService.initialized() || !transport.available())
    {
        m_state = SessionState::Error;
        m_error = "Steam lobby or networking sockets unavailable.";
        return;
    }
    if (!transport.startHosting() || !steamService.createLobby())
    {
        transport.disconnect();
        m_state = SessionState::Error;
        m_error = "Failed to start host lobby.";
        return;
    }
    m_state = SessionState::Joining;
}

void MultiplayerManager::joinLobby(
    SteamService& steamService,
    IMultiplayerTransport& transport,
    std::uint64_t lobbyId)
{
    m_error.clear();
    if (m_state != SessionState::Offline && m_state != SessionState::Error)
    {
        transport.disconnect();
        m_remotePlayers.clear();
        m_remoteEntities.clear();
    }
    if (!steamService.initialized() || lobbyId == 0 || !steamService.joinLobby(lobbyId))
    {
        m_state = SessionState::Error;
        m_error = "Failed to join lobby.";
        return;
    }
    m_state = SessionState::Joining;
}

void MultiplayerManager::leaveSession(SteamService& steamService, IMultiplayerTransport& transport)
{
    m_state = SessionState::Leaving;
    transport.disconnect();
    steamService.leaveLobby();
    m_remotePlayers.clear();
    m_remoteEntities.clear();
    m_lastHostConnectAttemptSeconds = -100.0;
    m_state = SessionState::Offline;
}

const char* MultiplayerManager::stateName(SessionState state)
{
    switch (state)
    {
    case SessionState::Offline: return "Offline";
    case SessionState::Hosting: return "Hosting";
    case SessionState::Joining: return "Joining";
    case SessionState::InLobby: return "InLobby";
    case SessionState::Leaving: return "Leaving";
    case SessionState::Error: return "Error";
    }
    return "Unknown";
}

void MultiplayerManager::handleTransportEvent(
    const IMultiplayerTransport::Event& event,
    SteamService& steamService,
    IMultiplayerTransport& transport,
    LightingSystem& lightingSystem,
    double nowSeconds,
    float deltaTimeSeconds)
{
    if (event.type == IMultiplayerTransport::EventType::Connected)
    {
        sendHello(transport, event.peerId, steamService, nowSeconds);
        if (m_state == SessionState::Hosting)
        {
            MultiplayerProtocol::Membership membership{};
            membership.header.type = MultiplayerProtocol::MessageType::Membership;
            membership.header.sequence = nextSequence();
            membership.header.senderTimeSeconds = nowSeconds;
            membership.steamId = event.peerId;
            membership.joined = 1;
            broadcastReliable(transport, &membership, sizeof(membership));
            sendHostTimeSync(transport, lightingSystem, nowSeconds, true);
        }
        return;
    }

    if (event.type == IMultiplayerTransport::EventType::Disconnected)
    {
        m_remotePlayers.erase(event.peerId);
        if (m_state == SessionState::Hosting)
        {
            MultiplayerProtocol::Membership membership{};
            membership.header.type = MultiplayerProtocol::MessageType::Membership;
            membership.header.sequence = nextSequence();
            membership.header.senderTimeSeconds = nowSeconds;
            membership.steamId = event.peerId;
            membership.joined = 0;
            broadcastReliable(transport, &membership, sizeof(membership), event.peerId);
        }
        return;
    }

    handleMessage(event.peerId, event.payload, steamService, transport, lightingSystem, nowSeconds, deltaTimeSeconds);
}

void MultiplayerManager::handleMessage(
    std::uint64_t peerId,
    const std::vector<std::uint8_t>& payload,
    SteamService& steamService,
    IMultiplayerTransport& transport,
    LightingSystem& lightingSystem,
    double nowSeconds,
    float deltaTimeSeconds)
{
    if (payload.size() < sizeof(MultiplayerProtocol::Header))
    {
        return;
    }

    MultiplayerProtocol::Header header{};
    std::memcpy(&header, payload.data(), sizeof(header));
    if (header.magic != MultiplayerProtocol::kMagic || header.version != MultiplayerProtocol::kVersion)
    {
        return;
    }

    switch (header.type)
    {
    case MultiplayerProtocol::MessageType::Hello:
    {
        MultiplayerProtocol::Hello hello{};
        if (readMessage(payload, hello))
        {
            RemotePlayerState& remote = m_remotePlayers[hello.steamId];
            remote.steamId = hello.steamId;
            remote.personaName = hello.personaName[0] != '\0'
                ? std::string(hello.personaName)
                : steamService.personaNameFor(hello.steamId);
        }
        break;
    }
    case MultiplayerProtocol::MessageType::PlayerState:
    {
        MultiplayerProtocol::PlayerState state{};
        if (!readMessage(payload, state) ||
            state.steamId == steamService.userId() ||
            (m_state == SessionState::Hosting && state.steamId != peerId) ||
            (m_state == SessionState::InLobby && peerId != steamService.currentLobbyOwnerId()))
        {
            break;
        }

        RemotePlayerState& remote = m_remotePlayers[state.steamId];
        remote.steamId = state.steamId;
        if (remote.personaName.empty())
        {
            remote.personaName = steamService.personaNameFor(state.steamId);
        }
        remote.lastPosition = fromNetworkPosition(state.position);
        remote.velocity = { state.velocityX, state.velocityY, state.velocityZ };
        remote.yawRadians = state.yawRadians;
        remote.lastReceiveTimeSeconds = nowSeconds;
        ++m_receivedPlayerStateCount;

        if (m_state == SessionState::Hosting)
        {
            broadcastUnreliable(transport, payload.data(), payload.size(), peerId);
        }
        break;
    }
    case MultiplayerProtocol::MessageType::TimeSync:
    {
        MultiplayerProtocol::TimeSync sync{};
        if (readMessage(payload, sync) &&
            m_state == SessionState::InLobby &&
            peerId == steamService.currentLobbyOwnerId())
        {
            applyTimeSync(sync, lightingSystem, deltaTimeSeconds);
        }
        break;
    }
    case MultiplayerProtocol::MessageType::Membership:
    {
        MultiplayerProtocol::Membership membership{};
        if (readMessage(payload, membership) &&
            (m_state == SessionState::Hosting || peerId == steamService.currentLobbyOwnerId()) &&
            membership.joined == 0)
        {
            m_remotePlayers.erase(membership.steamId);
        }
        break;
    }
    case MultiplayerProtocol::MessageType::Ping:
    {
        MultiplayerProtocol::Pong pong{};
        pong.header.type = MultiplayerProtocol::MessageType::Pong;
        pong.header.sequence = nextSequence();
        pong.header.senderTimeSeconds = nowSeconds;
        transport.sendReliable(peerId, &pong, sizeof(pong));
        break;
    }
    case MultiplayerProtocol::MessageType::Pong:
        break;
    }
}

void MultiplayerManager::sendHello(
    IMultiplayerTransport& transport,
    std::uint64_t peerId,
    SteamService& steamService,
    double nowSeconds)
{
    MultiplayerProtocol::Hello hello{};
    hello.header.type = MultiplayerProtocol::MessageType::Hello;
    hello.header.sequence = nextSequence();
    hello.header.senderTimeSeconds = nowSeconds;
    hello.steamId = steamService.userId();
    copyPersona(hello.personaName, steamService.personaName());
    transport.sendReliable(peerId, &hello, sizeof(hello));
}

void MultiplayerManager::sendLocalPlayerState(
    IMultiplayerTransport& transport,
    const PlayerPawn& localPlayer,
    SteamService& steamService,
    double nowSeconds)
{
    if (steamService.userId() == 0 || nowSeconds - m_lastPlayerSendSeconds < kPlayerSendIntervalSeconds)
    {
        return;
    }

    MultiplayerProtocol::PlayerState state{};
    state.header.type = MultiplayerProtocol::MessageType::PlayerState;
    state.header.sequence = nextSequence();
    state.header.senderTimeSeconds = nowSeconds;
    state.steamId = steamService.userId();
    state.position = toNetworkPosition(localPlayer.position);
    state.velocityX = static_cast<float>(localPlayer.velocity.x);
    state.velocityY = static_cast<float>(localPlayer.velocity.y);
    state.velocityZ = static_cast<float>(localPlayer.velocity.z);
    state.yawRadians = static_cast<float>(localPlayer.yawRadians);

    bool sent = false;
    if (m_state == SessionState::Hosting)
    {
        const std::vector<std::uint64_t> peers = transport.connectedPeers();
        if (peers.empty())
        {
            m_lastPlayerSendSeconds = nowSeconds;
            return;
        }
        for (std::uint64_t peerId : peers)
        {
            sent = transport.sendUnreliable(peerId, &state, sizeof(state)) || sent;
        }
    }
    else
    {
        sent = transport.sendUnreliable(steamService.currentLobbyOwnerId(), &state, sizeof(state));
    }
    if (!sent)
    {
        return;
    }
    ++m_sentPlayerStateCount;
    m_lastPlayerSendSeconds = nowSeconds;
}

void MultiplayerManager::sendHostTimeSync(
    IMultiplayerTransport& transport,
    const LightingSystem& lightingSystem,
    double nowSeconds,
    bool reliable)
{
    if (transport.connectedPeers().empty())
    {
        m_lastTimeSyncSeconds = nowSeconds;
        return;
    }

    MultiplayerProtocol::TimeSync sync{};
    sync.header.type = MultiplayerProtocol::MessageType::TimeSync;
    sync.header.sequence = nextSequence();
    sync.header.senderTimeSeconds = nowSeconds;
    sync.timeOfDayHours = lightingSystem.sun().timeOfDayHours;
    sync.dayLengthSeconds = lightingSystem.sun().dayLengthSeconds;
    sync.timeFactor = lightingSystem.sun().timeFactor;
    sync.timeRevision = ++m_timeRevision;
    if (reliable)
    {
        broadcastReliable(transport, &sync, sizeof(sync));
    }
    else
    {
        broadcastUnreliable(transport, &sync, sizeof(sync));
    }
    m_lastTimeSyncSeconds = nowSeconds;
}

void MultiplayerManager::applyTimeSync(
    const MultiplayerProtocol::TimeSync& sync,
    LightingSystem& lightingSystem,
    float deltaTimeSeconds)
{
    LightingSystem::SunLight& sun = lightingSystem.sun();
    sun.dayLengthSeconds = std::max(sync.dayLengthSeconds, 0.1f);
    sun.timeFactor = sync.timeFactor;

    m_timeSyncDriftHours = wrapHourDelta(sun.timeOfDayHours, sync.timeOfDayHours);
    if (std::abs(m_timeSyncDriftHours) >= kTimeSnapDriftHours)
    {
        sun.timeOfDayHours = sync.timeOfDayHours;
    }
    else
    {
        const float blend = std::clamp(deltaTimeSeconds * kTimeSmoothRate, 0.01f, 0.25f);
        sun.timeOfDayHours = wrapHour(sun.timeOfDayHours + (m_timeSyncDriftHours * blend));
    }
    m_timeRevision = sync.timeRevision;
}

void MultiplayerManager::rebuildRemoteEntities(double nowSeconds, float deltaTimeSeconds)
{
    m_remoteEntities.clear();
    for (auto& [steamId, remote] : m_remotePlayers)
    {
        if (steamId == 0 || remote.lastReceiveTimeSeconds <= 0.0)
        {
            continue;
        }

        const double elapsed = std::clamp(nowSeconds - remote.lastReceiveTimeSeconds, 0.0, kPredictionClampSeconds);
        const Position predicted = remote.lastPosition.translated(remote.velocity * elapsed);
        if (!remote.hasDrawPosition)
        {
            remote.smoothedDrawPosition = predicted;
            remote.hasDrawPosition = true;
        }
        else
        {
            const glm::dvec3 current = remote.smoothedDrawPosition.worldPosition();
            const glm::dvec3 target = predicted.worldPosition();
            const glm::dvec3 delta = target - current;
            if (glm::length(delta) > kPredictionSnapMeters)
            {
                remote.smoothedDrawPosition = predicted;
            }
            else
            {
                const double blend = std::clamp(static_cast<double>(deltaTimeSeconds) * kPositionSmoothRate, 0.0, 1.0);
                remote.smoothedDrawPosition = Position(0, 0, current + (delta * blend));
            }
        }

        m_remoteEntities.push_back({
            .steamId = steamId,
            .personaName = remote.personaName,
            .drawPosition = remote.smoothedDrawPosition,
            .yawRadians = static_cast<float>(remote.yawRadians),
            .color = colorForSteamId(steamId),
            .packetAgeSeconds = static_cast<float>(nowSeconds - remote.lastReceiveTimeSeconds),
        });
    }
}

void MultiplayerManager::refreshDebugSnapshot(
    SteamService& steamService,
    IMultiplayerTransport& transport,
    double nowSeconds)
{
    float newestAge = 0.0f;
    bool hasPacket = false;
    for (const auto& [steamId, remote] : m_remotePlayers)
    {
        (void)steamId;
        if (remote.lastReceiveTimeSeconds <= 0.0)
        {
            continue;
        }
        const float age = static_cast<float>(nowSeconds - remote.lastReceiveTimeSeconds);
        newestAge = hasPacket ? std::min(newestAge, age) : age;
        hasPacket = true;
    }

    m_debugSnapshot = {
        .state = m_state,
        .lobbyId = steamService.currentLobbyId(),
        .hostId = steamService.currentLobbyOwnerId(),
        .connectionCount = transport.connectionCount(),
        .remoteEntityCount = m_remoteEntities.size(),
        .newestPacketAgeSeconds = hasPacket ? newestAge : 0.0f,
        .sentPlayerStateCount = m_sentPlayerStateCount,
        .receivedPlayerStateCount = m_receivedPlayerStateCount,
        .timeSyncDriftHours = m_timeSyncDriftHours,
        .timeRevision = m_timeRevision,
        .connectionState = transport.debugConnectionState(),
        .error = m_error,
    };
}

void MultiplayerManager::refreshLobbyDrivenState(
    SteamService& steamService,
    IMultiplayerTransport& transport,
    double nowSeconds)
{
    if (m_state == SessionState::Offline && steamService.currentLobbyId() != 0)
    {
        m_state = SessionState::Joining;
    }

    if (m_state == SessionState::Joining && steamService.currentLobbyId() != 0 && !steamService.lobbyRequestPending())
    {
        if (steamService.currentLobbyOwnerId() == steamService.userId())
        {
            m_state = SessionState::Hosting;
        }
        else
        {
            m_state = SessionState::InLobby;
            if (!transport.hasPeer(steamService.currentLobbyOwnerId()))
            {
                transport.connectToHost(steamService.currentLobbyOwnerId());
                m_lastHostConnectAttemptSeconds = nowSeconds;
            }
        }
    }

    if (m_state == SessionState::InLobby &&
        steamService.currentLobbyOwnerId() != 0 &&
        !transport.hasPeer(steamService.currentLobbyOwnerId()) &&
        nowSeconds - m_lastHostConnectAttemptSeconds >= kHostConnectRetrySeconds)
    {
        transport.connectToHost(steamService.currentLobbyOwnerId());
        m_lastHostConnectAttemptSeconds = nowSeconds;
    }

    if ((m_state == SessionState::Hosting || m_state == SessionState::InLobby) &&
        steamService.currentLobbyId() == 0)
    {
        m_state = SessionState::Offline;
        transport.disconnect();
        m_remotePlayers.clear();
    }
}

void MultiplayerManager::broadcastReliable(
    IMultiplayerTransport& transport,
    const void* data,
    std::size_t size,
    std::uint64_t exceptPeer)
{
    for (std::uint64_t peerId : transport.connectedPeers())
    {
        if (peerId != exceptPeer)
        {
            transport.sendReliable(peerId, data, size);
        }
    }
}

void MultiplayerManager::broadcastUnreliable(
    IMultiplayerTransport& transport,
    const void* data,
    std::size_t size,
    std::uint64_t exceptPeer)
{
    for (std::uint64_t peerId : transport.connectedPeers())
    {
        if (peerId != exceptPeer)
        {
            transport.sendUnreliable(peerId, data, size);
        }
    }
}

glm::vec3 MultiplayerManager::colorForSteamId(std::uint64_t steamId) const
{
    const std::uint32_t r = static_cast<std::uint32_t>((steamId >> 0) & 0xffu);
    const std::uint32_t g = static_cast<std::uint32_t>((steamId >> 16) & 0xffu);
    const std::uint32_t b = static_cast<std::uint32_t>((steamId >> 32) & 0xffu);
    return {
        0.35f + (static_cast<float>(r) / 255.0f) * 0.55f,
        0.35f + (static_cast<float>(g) / 255.0f) * 0.55f,
        0.35f + (static_cast<float>(b) / 255.0f) * 0.55f,
    };
}
