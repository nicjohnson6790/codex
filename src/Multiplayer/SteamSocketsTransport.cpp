#include "Multiplayer/SteamSocketsTransport.hpp"

#include <algorithm>
#include <cstring>

SteamSocketsTransport::SteamSocketsTransport(SteamService& steamService)
    : m_steamService(steamService)
{
}

SteamSocketsTransport::~SteamSocketsTransport()
{
    disconnect();
}

bool SteamSocketsTransport::available() const
{
#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    return m_steamService.initialized() && SteamNetworkingSockets() != nullptr;
#else
    return false;
#endif
}

std::vector<IMultiplayerTransport::PeerId> SteamSocketsTransport::connectedPeers() const
{
    std::vector<PeerId> peers;
    peers.reserve(m_connections.size());
    for (const Connection& connection : m_connections)
    {
        if (connection.connected && connection.peerId != 0)
        {
            peers.push_back(connection.peerId);
        }
    }
    return peers;
}

bool SteamSocketsTransport::hasPeer(PeerId peerId) const
{
    for (const Connection& connection : m_connections)
    {
        if (connection.peerId == peerId)
        {
            return true;
        }
    }
    return false;
}

bool SteamSocketsTransport::startHosting()
{
#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    if (!available())
    {
        return false;
    }

    disconnect();
    m_listenSocket = SteamNetworkingSockets()->CreateListenSocketP2P(0, 0, nullptr);
    m_debugConnectionState = m_listenSocket != k_HSteamListenSocket_Invalid ? "listening" : "listen failed";
    return m_listenSocket != k_HSteamListenSocket_Invalid;
#else
    return false;
#endif
}

bool SteamSocketsTransport::connectToHost(PeerId hostId)
{
#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    if (!available() || hostId == 0)
    {
        return false;
    }

    disconnect();
    SteamNetworkingIdentity identity{};
    identity.SetSteamID64(hostId);
    const HSteamNetConnection connection = SteamNetworkingSockets()->ConnectP2P(identity, 0, 0, nullptr);
    if (connection == k_HSteamNetConnection_Invalid)
    {
        m_debugConnectionState = "connect failed";
        return false;
    }

    rememberConnection(connection, hostId, "connecting", false);
    m_debugConnectionState = "connecting";
    return true;
#else
    (void)hostId;
    return false;
#endif
}

void SteamSocketsTransport::disconnect()
{
#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    if (m_steamService.initialized() && SteamNetworkingSockets() != nullptr)
    {
        for (const Connection& connection : m_connections)
        {
            if (connection.handle != k_HSteamNetConnection_Invalid)
            {
                SteamNetworkingSockets()->CloseConnection(
                    connection.handle,
                    0,
                    "disconnect",
                    false);
            }
        }
        if (m_listenSocket != k_HSteamListenSocket_Invalid)
        {
            SteamNetworkingSockets()->CloseListenSocket(m_listenSocket);
        }
    }
#endif

    m_connections.clear();
    m_queuedEvents.clear();
    m_listenSocket = 0;
    m_debugConnectionState = "offline";
}

bool SteamSocketsTransport::sendReliable(PeerId peerId, const void* data, std::size_t size)
{
#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    return sendWithFlags(peerId, data, size, k_nSteamNetworkingSend_Reliable);
#else
    (void)peerId;
    (void)data;
    (void)size;
    return false;
#endif
}

bool SteamSocketsTransport::sendUnreliable(PeerId peerId, const void* data, std::size_t size)
{
#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    return sendWithFlags(peerId, data, size, k_nSteamNetworkingSend_UnreliableNoDelay);
#else
    (void)peerId;
    (void)data;
    (void)size;
    return false;
#endif
}

void SteamSocketsTransport::pollEvents(std::vector<Event>& events)
{
    events.insert(events.end(), m_queuedEvents.begin(), m_queuedEvents.end());
    m_queuedEvents.clear();
    pollMessages(events);
}

void SteamSocketsTransport::queueEvent(Event event)
{
    m_queuedEvents.push_back(std::move(event));
}

void SteamSocketsTransport::pollMessages(std::vector<Event>& events)
{
#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    if (!available())
    {
        return;
    }

    for (const Connection& connection : m_connections)
    {
        if (!connection.connected)
        {
            continue;
        }

        SteamNetworkingMessage_t* messages[16]{};
        const int messageCount = SteamNetworkingSockets()->ReceiveMessagesOnConnection(
            connection.handle,
            messages,
            16);
        for (int messageIndex = 0; messageIndex < messageCount; ++messageIndex)
        {
            SteamNetworkingMessage_t* message = messages[messageIndex];
            Event event{};
            event.type = EventType::Message;
            event.peerId = connection.peerId;
            event.payload.resize(static_cast<std::size_t>(std::max(message->m_cbSize, 0)));
            std::memcpy(event.payload.data(), message->m_pData, event.payload.size());
            events.push_back(std::move(event));
            message->Release();
        }
    }
#endif
}

void SteamSocketsTransport::rememberConnection(std::uint32_t handle, PeerId peerId, std::string state, bool connected)
{
    for (Connection& connection : m_connections)
    {
        if (connection.handle == handle)
        {
            connection.peerId = peerId;
            connection.state = std::move(state);
            connection.connected = connected;
            return;
        }
    }

    m_connections.push_back({
        .handle = handle,
        .peerId = peerId,
        .state = std::move(state),
        .connected = connected,
    });
}

void SteamSocketsTransport::forgetConnection(std::uint32_t handle)
{
    m_connections.erase(
        std::remove_if(
            m_connections.begin(),
            m_connections.end(),
            [handle](const Connection& connection) { return connection.handle == handle; }),
        m_connections.end());
}

std::uint32_t SteamSocketsTransport::connectionForPeer(PeerId peerId) const
{
    for (const Connection& connection : m_connections)
    {
        if (connection.connected && connection.peerId == peerId)
        {
            return connection.handle;
        }
    }
    return 0;
}

bool SteamSocketsTransport::sendWithFlags(PeerId peerId, const void* data, std::size_t size, int flags)
{
#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
    if (!available() || data == nullptr || size == 0)
    {
        return false;
    }

    const HSteamNetConnection connection = connectionForPeer(peerId);
    if (connection == k_HSteamNetConnection_Invalid)
    {
        return false;
    }

    return SteamNetworkingSockets()->SendMessageToConnection(
        connection,
        data,
        static_cast<std::uint32_t>(size),
        flags,
        nullptr) == k_EResultOK;
#else
    (void)peerId;
    (void)data;
    (void)size;
    (void)flags;
    return false;
#endif
}

#if defined(TERRAIN_SANDBOX_ENABLE_STEAM)
void SteamSocketsTransport::onConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* event)
{
    if (event == nullptr || SteamNetworkingSockets() == nullptr)
    {
        return;
    }

    const HSteamNetConnection connection = event->m_hConn;
    const std::uint64_t peerId = event->m_info.m_identityRemote.GetSteamID64();
    switch (event->m_info.m_eState)
    {
    case k_ESteamNetworkingConnectionState_Connecting:
        if (event->m_info.m_hListenSocket == m_listenSocket && m_listenSocket != k_HSteamListenSocket_Invalid)
        {
            SteamNetworkingSockets()->AcceptConnection(connection);
            rememberConnection(connection, peerId, "accepting", false);
            m_debugConnectionState = "accepting";
        }
        break;
    case k_ESteamNetworkingConnectionState_Connected:
        rememberConnection(connection, peerId, "connected", true);
        m_debugConnectionState = "connected";
        queueEvent({
            .type = EventType::Connected,
            .peerId = peerId,
            .detail = "connected",
        });
        break;
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        queueEvent({
            .type = EventType::Disconnected,
            .peerId = peerId,
            .detail = event->m_info.m_szEndDebug,
        });
        SteamNetworkingSockets()->CloseConnection(connection, 0, nullptr, false);
        forgetConnection(connection);
        if (!connectedPeers().empty())
        {
            m_debugConnectionState = "connected";
        }
        else
        {
            m_debugConnectionState = m_listenSocket != k_HSteamListenSocket_Invalid ? "listening" : "offline";
        }
        break;
    default:
        break;
    }
}
#endif
