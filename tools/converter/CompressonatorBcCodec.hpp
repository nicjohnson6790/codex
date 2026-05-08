#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

bool ConverterCompressBc3Rgba(
    std::span<const std::byte> rgbaPixels,
    std::uint32_t width,
    std::uint32_t height,
    std::vector<std::byte>* outCompressedBlocks,
    std::string* error);

bool ConverterCompressBc5Rg(
    std::span<const std::byte> rgbaPixels,
    std::uint32_t width,
    std::uint32_t height,
    std::vector<std::byte>* outCompressedBlocks,
    std::string* error);

[[nodiscard]] bool ConverterHasCompressonatorSupport();
[[nodiscard]] const char* ConverterCompressonatorUnavailableReason();
