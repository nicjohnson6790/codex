#pragma once

#include "PineTreePackConverter.hpp"

#include <filesystem>
#include <string>

bool ImportTextureFolder(
    const std::filesystem::path& textureRoot,
    ImportedPack* outPack,
    std::string* error);
