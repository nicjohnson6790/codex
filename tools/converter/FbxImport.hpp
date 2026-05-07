#pragma once

#include "PineTreePackConverter.hpp"

#include <filesystem>
#include <string>

bool ImportFbxFolder(
    const std::filesystem::path& fbxRoot,
    ImportedPack* outPack,
    std::size_t* outFbxFileCount,
    std::string* error);
