#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace HeightmapTileFilter
{

bool Encode(std::span<const std::int16_t> samples, std::vector<std::byte>* output, std::string* error);
bool Decode(std::span<const std::byte> input, std::vector<std::int16_t>* samples, std::string* error);
bool RunSelfTests(std::string* error);

} // namespace HeightmapTileFilter
