#pragma once

#include "RuntimeAssetFormat.hpp"

#include <array>
#include <cstdint>

namespace RuntimeAssets
{

constexpr std::uint32_t kHeightmapPackMagic = MakeMagic('H', 'M', 'A', 'P');
constexpr std::uint32_t kHeightmapFormatVersion = 2;
constexpr std::uint32_t kHeightmapProjectionVersion = 3;
constexpr std::uint32_t kHeightmapTileResolution = 256;
constexpr std::uint32_t kHeightmapTileStride = 255;
constexpr std::uint32_t kHeightmapTileSampleCount = 256u * 256u;
constexpr std::uint32_t kHeightmapFilteredTileBytes = kHeightmapTileSampleCount * sizeof(std::int16_t);
constexpr double kHeightmapTilePhysicalSizeMeters = 524288.0;
constexpr std::int16_t kHeightmapInvalidHeight = INT16_MIN;

enum class HeightSampleType : std::uint32_t
{
    SignedInt16Meters = 1,
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
    std::uint32_t tileCount = 0;
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
    std::int8_t tileX = 0;
    std::int8_t tileY = 0;
    std::uint16_t flags = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
    std::uint32_t validSampleCount = 0;
    std::int16_t validMinHeight = 0;
    std::int16_t validMaxHeight = 0;
    std::uint32_t reserved = 0;
    std::uint64_t blobOffset = 0;
};
static_assert(sizeof(HeightmapTileRecord) == 32);

} // namespace RuntimeAssets
