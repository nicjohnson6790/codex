#include "HeightmapTileFilter.hpp"

#include "assets/RuntimeHeightmapFormat.hpp"

#include <array>
#include <bit>
#include <cstring>

namespace HeightmapTileFilter
{
namespace
{

constexpr std::uint16_t ZigZagEncode(std::uint16_t residualBits)
{
    const bool negative = (residualBits & 0x8000u) != 0;
    return negative
        ? static_cast<std::uint16_t>((static_cast<std::uint16_t>(~residualBits) << 1u) | 1u)
        : static_cast<std::uint16_t>(residualBits << 1u);
}

constexpr std::uint16_t ZigZagDecode(std::uint16_t folded)
{
    const std::uint16_t magnitude = static_cast<std::uint16_t>(folded >> 1u);
    return (folded & 1u) != 0
        ? static_cast<std::uint16_t>(~magnitude)
        : magnitude;
}

constexpr std::size_t RasterIndex(std::size_t traversalIndex)
{
    const std::size_t y = traversalIndex / RuntimeAssets::kHeightmapTileResolution;
    const std::size_t inRow = traversalIndex % RuntimeAssets::kHeightmapTileResolution;
    const std::size_t x = (y & 1u) == 0u
        ? inRow
        : RuntimeAssets::kHeightmapTileResolution - 1u - inRow;
    return y * RuntimeAssets::kHeightmapTileResolution + x;
}

} // namespace

bool Encode(std::span<const std::int16_t> samples, std::vector<std::byte>* output, std::string* error)
{
    if (samples.size() != RuntimeAssets::kHeightmapTileSampleCount)
    {
        if (error) *error = "height tile filter requires exactly 256x256 samples";
        return false;
    }

    output->assign(RuntimeAssets::kHeightmapFilteredTileBytes, std::byte{});
    std::uint16_t previous = std::bit_cast<std::uint16_t>(samples[RasterIndex(0)]);
    (*output)[0] = static_cast<std::byte>(previous & 0xffu);
    (*output)[1] = static_cast<std::byte>(previous >> 8u);
    constexpr std::size_t lowOffset = 2;
    constexpr std::size_t highOffset = 2 + RuntimeAssets::kHeightmapTileSampleCount - 1;
    for (std::size_t i = 1; i < RuntimeAssets::kHeightmapTileSampleCount; ++i)
    {
        const std::uint16_t current = std::bit_cast<std::uint16_t>(samples[RasterIndex(i)]);
        const std::uint16_t folded = ZigZagEncode(static_cast<std::uint16_t>(current - previous));
        (*output)[lowOffset + i - 1] = static_cast<std::byte>(folded & 0xffu);
        (*output)[highOffset + i - 1] = static_cast<std::byte>(folded >> 8u);
        previous = current;
    }
    return true;
}

bool Decode(std::span<const std::byte> input, std::vector<std::int16_t>* samples, std::string* error)
{
    if (input.size() != RuntimeAssets::kHeightmapFilteredTileBytes)
    {
        if (error) *error = "filtered height tile payload has the wrong size";
        return false;
    }
    samples->assign(RuntimeAssets::kHeightmapTileSampleCount, 0);
    std::uint16_t previous = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[0])) |
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[1]) << 8u);
    (*samples)[RasterIndex(0)] = std::bit_cast<std::int16_t>(previous);
    constexpr std::size_t lowOffset = 2;
    constexpr std::size_t highOffset = 2 + RuntimeAssets::kHeightmapTileSampleCount - 1;
    for (std::size_t i = 1; i < RuntimeAssets::kHeightmapTileSampleCount; ++i)
    {
        const std::uint16_t folded = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[lowOffset + i - 1])) |
            static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[highOffset + i - 1]) << 8u);
        previous = static_cast<std::uint16_t>(previous + ZigZagDecode(folded));
        (*samples)[RasterIndex(i)] = std::bit_cast<std::int16_t>(previous);
    }
    return true;
}

bool RunSelfTests(std::string* error)
{
    std::vector<std::int16_t> source(RuntimeAssets::kHeightmapTileSampleCount);
    const std::array<std::int16_t, 16> cases{
        0, 1, -1, 2, -2, INT16_MIN, INT16_MAX, 0,
        INT16_MIN, 42, INT16_MAX, -42, -32767, 32767, 1234, -1234
    };
    for (std::size_t i = 0; i < source.size(); ++i)
        source[i] = cases[i % cases.size()];
    std::vector<std::byte> encoded;
    std::vector<std::int16_t> decoded;
    if (!Encode(source, &encoded, error) || !Decode(encoded, &decoded, error)) return false;
    if (source != decoded)
    {
        if (error) *error = "height filter edge-case round-trip failed";
        return false;
    }
    for (std::uint32_t bits = 0; bits <= 0xffffu; ++bits)
    {
        const auto value = static_cast<std::uint16_t>(bits);
        if (ZigZagDecode(ZigZagEncode(value)) != value)
        {
            if (error) *error = "16-bit ZigZag exhaustive round-trip failed";
            return false;
        }
    }
    return true;
}

} // namespace HeightmapTileFilter
