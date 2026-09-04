#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct JapanDem10ConversionConfig
{
    std::filesystem::path sourceRoot;
    std::filesystem::path etopoIndex;
    std::filesystem::path outputRoot;
    bool verbose = false;
    bool selfTestOnly = false;
};

struct JapanDem10ConversionSummary
{
    std::uint64_t sourceFiles = 0;
    std::uint64_t candidateTiles = 0;
    std::uint64_t storedTiles = 0;
    std::uint64_t omittedTiles = 0;
    std::uint64_t validSamples = 0;
};

class JapanDem10Converter
{
public:
    bool run(const JapanDem10ConversionConfig& config, JapanDem10ConversionSummary* summary, std::string* error);
};
