#pragma once

#include "RuntimeHeightmapFormat.hpp"
#include <algorithm>
#include <cmath>
#include <istream>
#include <string>
#include <vector>

namespace RuntimeAssets
{
// Shared by runtime, converter sampling, previews, and offline index migration.
inline bool ReadHeightmapIndex(std::istream& input, HeightmapPackHeader& header,
                              std::vector<HeightmapTileRecord>& table, std::string* error)
{
    auto fail = [&](const char* text) { if (error) *error = text; return false; };
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(&header), sizeof(header))) return fail("could not read heightmap header");
    if (header.magic != kHeightmapPackMagic || header.version != kHeightmapFormatVersion)
        return fail("unsupported heightmap format; migrate v3 with heightmap-reindex or regenerate ETOPO, then Japan DEM10");
    if (header.headerSize != sizeof(header) || header.tileRecordOffset != sizeof(header) ||
        header.fileSize != kHeightmapIndexBytes || header.tileCount > kHeightmapTileTableCount ||
        header.tileResolution != kHeightmapTileResolution || header.tileStride != kHeightmapTileStride ||
        header.sampleType != static_cast<std::uint32_t>(HeightSampleType::QuantizedInt16ScaleBias) ||
        header.filterType != static_cast<std::uint32_t>(HeightFilterType::SerpentineDeltaZigZagBytePlanes) ||
        header.compressionType != static_cast<std::uint32_t>(CompressionType::Lz4) ||
        std::find(header.dataFilename.begin(), header.dataFilename.end(), '\0') == header.dataFilename.end())
        return fail("invalid spatial heightmap index header");
    input.seekg(0, std::ios::end);
    if (input.tellg() != static_cast<std::streamoff>(kHeightmapIndexBytes)) return fail("heightmap index must contain exactly 256x256 records");
    input.seekg(header.tileRecordOffset);
    table.resize(kHeightmapTileTableCount);
    if (!input.read(reinterpret_cast<char*>(table.data()), kHeightmapTileTableBytes)) return fail("truncated heightmap tile table");
    std::uint32_t present = 0;
    for (std::uint32_t slot = 0; slot < kHeightmapTileTableCount; ++slot)
    {
        const auto& record = table[slot];
        if (!record.compressedSize)
        {
            if (record.tileX || record.tileY || record.flags || record.uncompressedSize || record.validSampleCount ||
                record.sampleScale != 0 || record.sampleBias != 0 || record.blobOffset)
                return fail("absent heightmap slot must be zero-filled");
            continue;
        }
        if (HeightmapTileTableIndex(record.tileX, record.tileY) != slot)
            return fail("heightmap tile coordinates do not match spatial slot");
        if (!std::isfinite(record.sampleScale) || record.sampleScale < 0 || !std::isfinite(record.sampleBias) ||
            record.compressedSize > kHeightmapFilteredTileBytes || record.uncompressedSize != kHeightmapFilteredTileBytes ||
            record.validSampleCount == 0 || record.validSampleCount > kHeightmapTileSampleCount)
            return fail("invalid heightmap tile metadata");
        ++present;
    }
    return present == header.tileCount || fail("heightmap present-tile count does not match table");
}
}
