#pragma once

#include <cstdint>
#include <string>
#include <vector>

class IMultiplayerTransport
{
public:
    using PeerId = std::uint64_t;

    enum class EventType
    {
        Connected,
        Disconnected,
        Message,
    };

    struct Event
    {
        EventType type = EventType::Message;
        PeerId peerId = 0;
        std::vector<std::uint8_t> payload;
        std::string detail;
    };

    virtual ~IMultiplayerTransport() = default;

    [[nodiscard]] virtual bool available() const = 0;
    [[nodiscard]] virtual bool hosting() const = 0;
    [[nodiscard]] virtual std::size_t connectionCount() const = 0;
    [[nodiscard]] virtual bool hasPeer(PeerId peerId) const = 0;
    [[nodiscard]] virtual std::vector<PeerId> connectedPeers() const = 0;
    [[nodiscard]] virtual std::string debugConnectionState() const = 0;

    virtual bool startHosting() = 0;
    virtual bool connectToHost(PeerId hostId) = 0;
    virtual void disconnect() = 0;
    virtual bool sendReliable(PeerId peerId, const void* data, std::size_t size) = 0;
    virtual bool sendUnreliable(PeerId peerId, const void* data, std::size_t size) = 0;
    virtual void pollEvents(std::vector<Event>& events) = 0;
};
