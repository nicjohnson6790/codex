#pragma once

#include "PineTreePackConverter.hpp"

#include <filesystem>
#include <string>
#include <vector>

bool WriteTexBin(
    const ImportedPack& pack,
    const std::filesystem::path& outputPath,
    std::vector<RuntimeAssets::TextureBlobRecord>* outTextureBlobs,
    std::uint64_t* outFileSize,
    std::string* error);
