#pragma once

#include "HeightmapQuantization.hpp"
#include "assets/RuntimeHeightmapIndex.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>

// Explicit offline migration of legacy fast-LZ4 tiles. Original blobs remain
// intact; an index backup makes the operation reversible without copying GBs.
inline void RepackHeightmap(const std::filesystem::path &indexPath)
{
    std::ifstream index(indexPath, std::ios::binary);
    RuntimeAssets::HeightmapPackHeader header{};
    index.read(reinterpret_cast<char *>(&header), sizeof(header));
    if (!index || header.magic != RuntimeAssets::kHeightmapPackMagic || (header.version != 3 && header.version != RuntimeAssets::kHeightmapFormatVersion) ||
        header.compressionType != static_cast<unsigned>(RuntimeAssets::CompressionType::Lz4))
        throw std::runtime_error("repack requires a version 3 or 4 LZ4 heightmap pack");
    if (header.tileCount > RuntimeAssets::kHeightmapTileTableCount) throw std::runtime_error("invalid heightmap tile count");
    std::vector<RuntimeAssets::HeightmapTileRecord> records(header.version == 3 ? header.tileCount : RuntimeAssets::kHeightmapTileTableCount);
    index.seekg(header.tileRecordOffset);
    index.read(reinterpret_cast<char *>(records.data()), records.size() * sizeof(records[0]));
    if (!index) throw std::runtime_error("cannot read heightmap index");
    index.close();
    const auto backup = std::filesystem::path(indexPath.string() + ".before-bounded");
    if (std::none_of(records.begin(), records.end(), [](const auto &r) { return r.compressedSize > RuntimeAssets::kHeightmapFilteredTileBytes; }))
    {
        std::cout << indexPath << ": already bounded\n";
        return;
    }
    if (std::filesystem::exists(backup)) throw std::runtime_error("index backup already exists; inspect it before repacking");
    std::fstream data(indexPath.parent_path() / header.dataFilename.data(), std::ios::binary | std::ios::in | std::ios::out);
    unsigned changed = 0;
    for (auto &record : records)
    {
        if (record.compressedSize <= RuntimeAssets::kHeightmapFilteredTileBytes) continue;
        if (record.compressedSize > static_cast<unsigned>(LZ4_compressBound(RuntimeAssets::kHeightmapFilteredTileBytes)) ||
            record.uncompressedSize != RuntimeAssets::kHeightmapFilteredTileBytes)
            throw std::runtime_error("invalid legacy tile size");
        std::vector<std::byte> input(record.compressedSize), filtered, compressed, restored;
        data.seekg(record.blobOffset);
        data.read(reinterpret_cast<char *>(input.data()), input.size());
        std::string error;
        if (!data || !RuntimeAssets::DecompressBytes(RuntimeAssets::CompressionType::Lz4, input, record.uncompressedSize, &filtered, &error) ||
            !HeightmapQuantization::CompressFiltered(filtered, compressed, &error) ||
            !RuntimeAssets::DecompressBytes(RuntimeAssets::CompressionType::Lz4, compressed, filtered.size(), &restored, &error) ||
            filtered != restored)
            throw std::runtime_error("lossless repack verification failed: " + error);
        data.seekp(0, std::ios::end);
        record.blobOffset = static_cast<std::uint64_t>(data.tellp());
        record.compressedSize = static_cast<std::uint32_t>(compressed.size());
        data.write(reinterpret_cast<const char *>(compressed.data()), compressed.size());
        if (!data) throw std::runtime_error("cannot append repacked tile");
        ++changed;
    }
    data.flush();
    if (!data) throw std::runtime_error("cannot flush repacked data");
    data.close();
    const auto temporary = std::filesystem::path(indexPath.string() + ".repacked");
    std::filesystem::copy_file(indexPath, temporary, std::filesystem::copy_options::overwrite_existing);
    {
        std::fstream output(temporary, std::ios::binary | std::ios::in | std::ios::out);
        output.seekp(header.tileRecordOffset);
        output.write(reinterpret_cast<const char *>(records.data()), records.size() * sizeof(records[0]));
        output.flush();
        if (!output) throw std::runtime_error("cannot write repacked index");
    }
    std::filesystem::rename(indexPath, backup);
    try { std::filesystem::rename(temporary, indexPath); }
    catch (...) { std::filesystem::rename(backup, indexPath); throw; }
    std::cout << indexPath << ": losslessly repacked " << changed << " tiles; original index: " << backup << '\n';
}
