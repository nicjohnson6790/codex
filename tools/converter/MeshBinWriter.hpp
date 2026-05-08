#pragma once

#include "PineTreePackConverter.hpp"

#include <filesystem>
#include <string>
#include <vector>

bool WriteMeshBin(
    const ImportedPack& pack,
    const std::filesystem::path& outputPath,
    std::vector<RuntimeAssets::MeshBlobRecord>* outMeshBlobs,
    std::uint64_t* outFileSize,
    std::string* error);
