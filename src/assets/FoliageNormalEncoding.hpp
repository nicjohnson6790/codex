#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace RuntimeAssets
{
// Capture/filter scratch pixels contain signed tree-local XYZ in RGB UNORM.
// In particular, Z is data; reconstructing a positive hemisphere changes it.
inline std::array<float, 3> DecodeFoliageNormal(const std::byte* pixel)
{
    return {
        float(std::to_integer<std::uint8_t>(pixel[0])) / 127.5f - 1.0f,
        float(std::to_integer<std::uint8_t>(pixel[1])) / 127.5f - 1.0f,
        float(std::to_integer<std::uint8_t>(pixel[2])) / 127.5f - 1.0f};
}
}
