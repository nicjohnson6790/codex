#pragma once

#include "RuntimeAssetFormat.hpp"

#include <array>
#include <cstdint>

namespace RuntimeAssets
{

constexpr std::uint32_t kHeightmapPackMagic = MakeMagic('H', 'M', 'A', 'P');
constexpr std::uint32_t kHeightmapFormatVersion = 4;
constexpr std::uint32_t kHeightmapTileTableSide = 256;
constexpr std::uint32_t kHeightmapTileTableCount = kHeightmapTileTableSide * kHeightmapTileTableSide;
constexpr bool HeightmapTileInRange(std::int32_t x, std::int32_t y)
{
    return x >= -128 && x <= 127 && y >= -128 && y <= 127;
}
// Call only after checking the signed coordinate range; no wrapping at edges.
constexpr std::uint32_t HeightmapTileTableIndex(std::int32_t x, std::int32_t y)
{
    return static_cast<std::uint32_t>(y + 128) * kHeightmapTileTableSide + static_cast<std::uint32_t>(x + 128);
}
constexpr std::uint32_t kHeightmapProjectionVersion = 3;
constexpr std::uint32_t kHeightmapTileResolution = 256;
constexpr std::uint32_t kHeightmapTileStride = 255;
constexpr std::uint32_t kHeightmapTileSampleCount = 256u * 256u;
constexpr std::uint32_t kHeightmapFilteredTileBytes = kHeightmapTileSampleCount * sizeof(std::int16_t);
constexpr double kHeightmapTilePhysicalSizeMeters = 524288.0;
constexpr std::int16_t kHeightmapInvalidHeight = INT16_MIN;
constexpr std::int16_t kHeightmapExactZeroHeight = INT16_MIN + 1;

enum class HeightSampleType : std::uint32_t
{
    QuantizedInt16ScaleBias = 2,
};

enum class HeightFilterType : std::uint32_t
{
    SerpentineDeltaZigZagBytePlanes = 1,
};

struct HeightmapPackHeader
{
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint32_t headerSize = 0;
    std::uint32_t tileCount = 0; // Present tiles; v4 always stores 65,536 records.
    std::uint32_t tileResolution = 0;
    std::uint32_t tileStride = 0;
    std::uint32_t sampleType = 0;
    std::int32_t invalidHeight = 0;
    std::uint32_t filterType = 0;
    std::uint32_t filterVersion = 0;
    std::uint32_t compressionType = 0;
    double tilePhysicalSizeMeters = 0.0;
    double authalicRadiusMeters = 0.0;
    std::array<double, 3> orientationDegrees{}; // longitude, latitude, roll
    double atlasRotationDegrees = 0.0;
    std::array<double, 4> projectedBoundsMeters{}; // min X, min Y, max X, max Y
    std::uint32_t projectionVersion = 0;
    std::uint32_t projectionName = 0;
    std::uint64_t tileRecordOffset = 0;
    std::uint64_t fileSize = 0;
    std::array<char, 32> dataFilename{};
    std::array<char, 48> sourceDataset{};
};
static_assert(sizeof(HeightmapPackHeader) == 232);

struct HeightmapTileRecord
{
    // Coordinates are redundant validation for present records. Absent slots
    // are entirely zero, including compressedSize; offset zero may be present.
    std::int8_t tileX = 0;
    std::int8_t tileY = 0;
    std::uint16_t flags = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
    std::uint32_t validSampleCount = 0;
    float sampleScale = 0;
    float sampleBias = 0;
    std::uint64_t blobOffset = 0;
};
static_assert(sizeof(HeightmapTileRecord) == 32);
constexpr std::uint64_t kHeightmapTileTableBytes = std::uint64_t(kHeightmapTileTableCount) * sizeof(HeightmapTileRecord);
constexpr std::uint64_t kHeightmapIndexBytes = sizeof(HeightmapPackHeader) + kHeightmapTileTableBytes;

// Invalid samples retain the runtime's existing zero-contribution behavior.
inline float DecodeHeight(std::int16_t sample, float scale, float bias)
{
    if (sample == kHeightmapInvalidHeight || sample == kHeightmapExactZeroHeight) return 0.0f;
    return bias + static_cast<float>(sample) * scale;
}

} // namespace RuntimeAssets
