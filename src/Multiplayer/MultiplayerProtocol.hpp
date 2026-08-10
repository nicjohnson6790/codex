#pragma once

#include <cstdint>

namespace MultiplayerProtocol
{
constexpr std::uint32_t kMagic = 0x54534d50u;
constexpr std::uint16_t kVersion = 1;

enum class MessageType : std::uint16_t
{
    Hello = 1,
    PlayerState = 2,
    TimeSync = 3,
    Membership = 4,
    Ping = 5,
    Pong = 6,
};

#pragma pack(push, 1)
struct Header
{
    std::uint32_t magic = kMagic;
    std::uint16_t version = kVersion;
    MessageType type = MessageType::Hello;
    std::uint32_t sequence = 0;
    double senderTimeSeconds = 0.0;
};

struct NetworkPosition
{
    std::int64_t worldX = 0;
    std::int64_t worldY = 0;
    float localX = 0.0f;
    float localY = 0.0f;
    float localZ = 0.0f;
};

struct Hello
{
    Header header;
    std::uint64_t steamId = 0;
    char personaName[64]{};
};

struct PlayerState
{
    Header header;
    std::uint64_t steamId = 0;
    NetworkPosition position;
    float velocityX = 0.0f;
    float velocityY = 0.0f;
    float velocityZ = 0.0f;
    float yawRadians = 0.0f;
};

struct TimeSync
{
    Header header;
    float timeOfDayHours = 0.0f;
    float dayLengthSeconds = 0.0f;
    float timeFactor = 1.0f;
    std::uint32_t timeRevision = 0;
};

struct Membership
{
    Header header;
    std::uint64_t steamId = 0;
    std::uint8_t joined = 1;
};

struct Ping
{
    Header header;
};

struct Pong
{
    Header header;
};
#pragma pack(pop)
}
