#include "AssetBinWriter.hpp"

#include "../../src/assets/RuntimeAssetFormat.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

struct ByteWriter
{
    std::vector<std::byte> bytes;

    template <typename T>
    void appendValue(const T& value)
    {
        const std::size_t offset = bytes.size();
        bytes.resize(offset + sizeof(T));
        std::memcpy(bytes.data() + offset, &value, sizeof(T));
    }

    template <typename T>
    void appendSpan(std::span<const T> values)
    {
        if (!values.empty())
        {
            const std::size_t offset = bytes.size();
            bytes.resize(offset + values.size_bytes());
            std::memcpy(bytes.data() + offset, values.data(), values.size_bytes());
        }
    }

    void align(std::size_t alignment)
    {
        const std::size_t remainder = bytes.size() % alignment;
        if (remainder != 0)
        {
            bytes.resize(bytes.size() + (alignment - remainder), std::byte{});
        }
    }
};

std::uint32_t AppendString(std::string_view value, std::string* table)
{
    const std::uint32_t offset = static_cast<std::uint32_t>(table->size());
    table->append(value);
    table->push_back('\0');
    return offset;
}

bool WriteAllBytes(const std::filesystem::path& outputPath, const std::vector<std::byte>& bytes, std::string* error)
{
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        *error = "failed to open output file: " + outputPath.string();
        return false;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output)
    {
        *error = "failed to write output file: " + outputPath.string();
        return false;
    }
    return true;
}

} // namespace

bool WriteAssetBin(
    const ImportedPack& pack,
    std::span<const RuntimeAssets::MeshBlobRecord> meshBlobs,
    std::span<const RuntimeAssets::TextureBlobRecord> textureBlobs,
    std::string_view meshBinFileName,
    std::string_view texBinFileName,
    const std::filesystem::path& outputPath,
    std::uint64_t* outFileSize,
    std::string* error)
{
    std::vector<RuntimeAssets::AssetRecord> assetRecords;
    std::vector<RuntimeAssets::MaterialRecord> materialRecords;
    std::vector<RuntimeAssets::MeshRefRecord> meshRefs;
    std::string stringTable;

    if (meshBlobs.size() != pack.meshes.size())
    {
        *error = "mesh blob manifest count does not match mesh count";
        return false;
    }
    if (textureBlobs.size() != pack.textures.size())
    {
        *error = "texture blob manifest count does not match texture count";
        return false;
    }

    assetRecords.reserve(pack.assets.size());
    materialRecords.reserve(pack.materials.size());
    meshRefs.reserve(pack.meshes.size());

    for (const ImportedMaterial& material : pack.materials)
    {
        RuntimeAssets::MaterialRecord record = material.record;
        record.nameOffset = AppendString(material.sourceName, &stringTable);
        materialRecords.push_back(record);
    }

    for (const ImportedAsset& asset : pack.assets)
    {
        RuntimeAssets::AssetRecord record{};
        record.nameOffset = AppendString(asset.name, &stringTable);
        record.firstMeshRef = static_cast<std::uint32_t>(meshRefs.size());
        record.meshRefCount = static_cast<std::uint32_t>(asset.meshIndices.size());
        record.firstMaterial = asset.materialIndices.empty() ? 0u : asset.materialIndices.front();
        record.materialCount = static_cast<std::uint32_t>(asset.materialIndices.size());
        record.flags = 0;

        for (std::uint32_t meshIndex : asset.meshIndices)
        {
            const ImportedMesh& mesh = pack.meshes[meshIndex];
            RuntimeAssets::MeshRefRecord meshRef{};
            meshRef.meshIndex = meshIndex;
            meshRef.lodIndex = mesh.lodIndex;
            meshRef.maxDistanceMeters = mesh.lodMaxDistance;
            meshRef.flags = 0;
            meshRefs.push_back(meshRef);
        }

        assetRecords.push_back(record);
    }

    RuntimeAssets::AssetBinHeader header{};
    header.magic = RuntimeAssets::kAssetBinMagic;
    header.version = RuntimeAssets::kFormatVersion;
    header.flags = RuntimeAssets::kLittleEndianFlag;
    header.assetCount = static_cast<std::uint32_t>(assetRecords.size());
    header.materialCount = static_cast<std::uint32_t>(materialRecords.size());
    header.meshRefCount = static_cast<std::uint32_t>(meshRefs.size());
    header.meshBlobCount = static_cast<std::uint32_t>(meshBlobs.size());
    header.textureBlobCount = static_cast<std::uint32_t>(textureBlobs.size());
    header.meshBinPathOffset = AppendString(meshBinFileName, &stringTable);
    header.texBinPathOffset = AppendString(texBinFileName, &stringTable);

    ByteWriter writer;
    writer.appendValue(header);
    writer.align(8);
    header.assetRecordOffset = writer.bytes.size();
    writer.appendSpan(std::span<const RuntimeAssets::AssetRecord>(assetRecords));
    writer.align(8);
    header.materialRecordOffset = writer.bytes.size();
    writer.appendSpan(std::span<const RuntimeAssets::MaterialRecord>(materialRecords));
    writer.align(8);
    header.meshRefRecordOffset = writer.bytes.size();
    writer.appendSpan(std::span<const RuntimeAssets::MeshRefRecord>(meshRefs));
    writer.align(8);
    header.meshBlobRecordOffset = writer.bytes.size();
    writer.appendSpan(meshBlobs);
    writer.align(8);
    header.textureBlobRecordOffset = writer.bytes.size();
    writer.appendSpan(textureBlobs);
    writer.align(8);
    header.stringTableOffset = writer.bytes.size();
    header.stringTableSize = stringTable.size();
    writer.bytes.insert(
        writer.bytes.end(),
        reinterpret_cast<const std::byte*>(stringTable.data()),
        reinterpret_cast<const std::byte*>(stringTable.data() + stringTable.size()));
    header.fileSize = writer.bytes.size();
    std::memcpy(writer.bytes.data(), &header, sizeof(header));

    if (!WriteAllBytes(outputPath, writer.bytes, error))
    {
        return false;
    }

    *outFileSize = header.fileSize;
    return true;
}
