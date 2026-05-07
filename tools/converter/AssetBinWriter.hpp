#pragma once

#include "PineTreePackConverter.hpp"

#include <filesystem>
#include <string>

bool WriteAssetBin(
    const ImportedPack& pack,
    const std::string& packName,
    const std::filesystem::path& outputPath,
    std::uint64_t* outFileSize,
    std::string* error);
