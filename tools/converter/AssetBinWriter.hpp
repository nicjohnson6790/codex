#pragma once

#include "PineTreePackConverter.hpp"

#include <filesystem>
#include <span>
#include <string>

bool WriteAssetBin(
    const ImportedPack& pack,
    std::span<const RuntimeAssets::MeshBlobRecord> meshBlobs,
    std::span<const RuntimeAssets::TextureBlobRecord> textureBlobs,
    const std::string& packName,
    const std::filesystem::path& outputPath,
    std::uint64_t* outFileSize,
    std::string* error);
