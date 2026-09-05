#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct EtopoConversionConfig
{
    std::filesystem::path sourceTiff;
    std::filesystem::path outputRoot;
    bool verbose = false;
    bool selfTestOnly = false;
};

struct EtopoConversionSummary
{
    std::uint32_t sourceWidth = 0;
    std::uint32_t sourceHeight = 0;
    std::int32_t candidateTiles = 0;
    std::int32_t omittedTiles = 0;
    std::int32_t storedTiles = 0;
    std::int32_t partialTiles = 0;
    std::int32_t minTileX = 0, maxTileX = 0, minTileY = 0, maxTileY = 0;
    float minHeight = 0, maxHeight = 0;
    std::uint64_t rawBytes = 0;
    std::uint64_t filteredBytes = 0;
    std::uint64_t compressedBytes = 0;
};

class EtopoHeightmapConverter
{
public:
    bool run(const EtopoConversionConfig& config, EtopoConversionSummary* summary, std::string* error);
};
