#pragma once

#include "Multiplayer/IMultiplayerTransport.hpp"
#include "platform/SteamService.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
#include <steam/steam_api.h>
#endif

class SteamSocketsTransport final : public IMultiplayerTransport
{
public:
    explicit SteamSocketsTransport(SteamService& steamService);
    ~SteamSocketsTransport() override;

    [[nodiscard]] bool available() const override;
    [[nodiscard]] bool hosting() const override { return m_listenSocket != 0; }
    [[nodiscard]] std::size_t connectionCount() const override { return connectedPeers().size(); }
    [[nodiscard]] bool hasPeer(PeerId peerId) const override;
    [[nodiscard]] std::vector<PeerId> connectedPeers() const override;
    [[nodiscard]] std::string debugConnectionState() const override { return m_debugConnectionState; }

    bool startHosting() override;
    bool connectToHost(PeerId hostId) override;
    void disconnect() override;
    bool sendReliable(PeerId peerId, const void* data, std::size_t size) override;
    bool sendUnreliable(PeerId peerId, const void* data, std::size_t size) override;
    void pollEvents(std::vector<Event>& events) override;

private:
    struct Connection
    {
        std::uint32_t handle = 0;
        PeerId peerId = 0;
        std::string state;
        bool connected = false;
    };

    void queueEvent(Event event);
    void pollMessages(std::vector<Event>& events);
    void rememberConnection(std::uint32_t handle, PeerId peerId, std::string state, bool connected);
    void forgetConnection(std::uint32_t handle);
    [[nodiscard]] std::uint32_t connectionForPeer(PeerId peerId) const;
    bool sendWithFlags(PeerId peerId, const void* data, std::size_t size, int flags);

#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    STEAM_CALLBACK(SteamSocketsTransport, onConnectionStatusChanged, SteamNetConnectionStatusChangedCallback_t);
#endif

    SteamService& m_steamService;
    std::uint32_t m_listenSocket = 0;
    std::vector<Connection> m_connections;
    std::vector<Event> m_queuedEvents;
    std::string m_debugConnectionState = "offline";
};
