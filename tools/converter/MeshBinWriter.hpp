#pragma once

#include "PineTreePackConverter.hpp"

#include <filesystem>
#include <string>

bool WriteMeshBin(
    const ImportedPack& pack,
    const std::filesystem::path& outputPath,
    std::uint64_t* outFileSize,
    std::string* error);
