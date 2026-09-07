#pragma once

#include "assets/RuntimeHeightmapIndex.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

// Metadata-only migration: leave compressed heightbin bytes and offsets intact.
inline void ReindexHeightmap(const std::filesystem::path& path)
{
    using namespace RuntimeAssets;
    std::ifstream input(path, std::ios::binary);
    HeightmapPackHeader header{};
    if (!input.read(reinterpret_cast<char*>(&header), sizeof(header))) throw std::runtime_error("cannot read heightmap header");
    std::vector<HeightmapTileRecord> table(kHeightmapTileTableCount);
    std::string error;
    if (header.version == kHeightmapFormatVersion)
    {
        if (!ReadHeightmapIndex(input, header, table, &error)) throw std::runtime_error(error);
        std::cout << path << ": already spatial\n";
        return;
    }
    if (header.magic != kHeightmapPackMagic || header.version != 3 || header.tileCount > kHeightmapTileTableCount ||
        header.tileRecordOffset != sizeof(header) || header.headerSize != sizeof(header) ||
        header.fileSize != sizeof(header) + std::uint64_t(header.tileCount) * sizeof(HeightmapTileRecord) ||
        std::filesystem::file_size(path) != header.fileSize ||
        std::find(header.dataFilename.begin(), header.dataFilename.end(), '\0') == header.dataFilename.end())
        throw std::runtime_error("reindex requires a valid version 3 heightmap index");
    const auto dataSize = std::filesystem::file_size(path.parent_path() / header.dataFilename.data());
    for (std::uint32_t i = 0; i < header.tileCount; ++i)
    {
        HeightmapTileRecord record{};
        if (!input.read(reinterpret_cast<char*>(&record), sizeof(record))) throw std::runtime_error("truncated legacy index");
        auto& slot = table[HeightmapTileTableIndex(record.tileX, record.tileY)];
        if (slot.compressedSize || !record.compressedSize || record.blobOffset > dataSize ||
            record.compressedSize > dataSize - record.blobOffset)
            throw std::runtime_error("duplicate, absent, or out-of-bounds legacy tile");
        slot = record;
    }
    input.close();
    const auto backup = std::filesystem::path(path.string() + ".before-spatial");
    if (std::filesystem::exists(backup)) throw std::runtime_error("spatial index backup already exists");
    const auto temporary = std::filesystem::path(path.string() + ".spatial");
    header.version = kHeightmapFormatVersion;
    header.fileSize = kHeightmapIndexBytes;
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(&header), sizeof(header));
        output.write(reinterpret_cast<const char*>(table.data()), kHeightmapTileTableBytes);
        output.close();
        if (!output) throw std::runtime_error("cannot write spatial heightmap table");
    }
    {
        std::ifstream verify(temporary, std::ios::binary);
        if (!ReadHeightmapIndex(verify, header, table, &error)) throw std::runtime_error(error);
    }
    std::filesystem::rename(path, backup);
    try { std::filesystem::rename(temporary, path); }
    catch (...) { std::filesystem::rename(backup, path); throw; }
    std::cout << path << ": migrated " << header.tileCount << " present tiles to 256x256 table; heightbin unchanged\n";
}
