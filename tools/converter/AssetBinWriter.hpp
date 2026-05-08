#pragma once

#include "PineTreePackConverter.hpp"

#include <filesystem>
#include <span>
#include <string>

bool WriteAssetBin(
    const ImportedPack& pack,
    std::span<const RuntimeAssets::MeshBlobRecord> meshBlobs,
    std::span<const RuntimeAssets::TextureBlobRecord> textureBlobs,
    std::string_view meshBinFileName,
    std::string_view texBinFileName,
    const std::filesystem::path& outputPath,
    std::uint64_t* outFileSize,
    std::string* error);
