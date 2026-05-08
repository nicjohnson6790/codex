#pragma once

#include "RuntimeAssetFormat.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace RuntimeAssets
{

bool CompressBytes(
    CompressionType compressionType,
    std::span<const std::byte> source,
    std::vector<std::byte>* out,
    std::string* error);

bool DecompressBytes(
    CompressionType compressionType,
    std::span<const std::byte> source,
    std::size_t uncompressedSize,
    std::vector<std::byte>* out,
    std::string* error);

} // namespace RuntimeAssets
