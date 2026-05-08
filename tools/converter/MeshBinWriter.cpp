#include "MeshBinWriter.hpp"

#include "../../src/assets/RuntimeAssetCompression.hpp"
#include "../../src/assets/RuntimeAssetFormat.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
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
        if (values.empty())
        {
            return;
        }

        const std::size_t offset = bytes.size();
        const std::size_t byteCount = values.size_bytes();
        bytes.resize(offset + byteCount);
        std::memcpy(bytes.data() + offset, values.data(), byteCount);
    }

    void appendBytes(std::span<const std::byte> value)
    {
        bytes.insert(bytes.end(), value.begin(), value.end());
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

bool WriteMeshBin(
    const ImportedPack& pack,
    const std::filesystem::path& outputPath,
    std::vector<RuntimeAssets::MeshBlobRecord>* outMeshBlobs,
    std::uint64_t* outFileSize,
    std::string* error)
{
    std::vector<RuntimeAssets::MeshRecord> meshRecords;
    std::vector<RuntimeAssets::SubmeshRecord> submeshRecords;
    std::vector<RuntimeAssets::MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<RuntimeAssets::MeshBlobRecord> meshBlobs;
    std::string stringTable;

    meshRecords.reserve(pack.meshes.size());
    submeshRecords.reserve(pack.meshes.size());
    meshBlobs.reserve(pack.meshes.size());

    for (std::uint32_t meshIndex = 0; meshIndex < pack.meshes.size(); ++meshIndex)
    {
        const ImportedMesh& mesh = pack.meshes[meshIndex];
        RuntimeAssets::MeshRecord meshRecord{};
        meshRecord.nameOffset = AppendString(mesh.meshName, &stringTable);
        meshRecord.firstSubmesh = static_cast<std::uint32_t>(submeshRecords.size());
        meshRecord.submeshCount = 1;
        meshRecord.firstVertex = static_cast<std::uint32_t>(vertices.size());
        meshRecord.vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
        meshRecord.firstIndex = static_cast<std::uint32_t>(indices.size());
        meshRecord.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
        meshRecord.boundsMin = mesh.boundsMin;
        meshRecord.boundsMax = mesh.boundsMax;
        meshRecord.boundsSphereCenter = mesh.boundsCenter;
        meshRecord.boundsSphereRadius = mesh.boundsRadius;
        meshRecords.push_back(meshRecord);

        RuntimeAssets::SubmeshRecord submesh{};
        submesh.meshIndex = meshIndex;
        submesh.materialIndex = mesh.materialIndex;
        submesh.firstIndex = meshRecord.firstIndex;
        submesh.indexCount = meshRecord.indexCount;
        submesh.firstVertex = meshRecord.firstVertex;
        submesh.vertexCount = meshRecord.vertexCount;
        submeshRecords.push_back(submesh);

        vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        indices.insert(indices.end(), mesh.indices.begin(), mesh.indices.end());
    }

    RuntimeAssets::MeshBinHeader header{};
    header.magic = RuntimeAssets::kMeshBinMagic;
    header.version = RuntimeAssets::kFormatVersion;
    header.flags = RuntimeAssets::kLittleEndianFlag;
    header.meshCount = static_cast<std::uint32_t>(meshRecords.size());
    header.vertexCount = static_cast<std::uint32_t>(vertices.size());
    header.indexCount = static_cast<std::uint32_t>(indices.size());
    header.submeshCount = static_cast<std::uint32_t>(submeshRecords.size());

    ByteWriter writer;
    writer.appendValue(header);
    writer.align(8);
    header.meshRecordOffset = writer.bytes.size();
    writer.appendSpan(std::span<const RuntimeAssets::MeshRecord>(meshRecords));
    writer.align(8);
    header.submeshRecordOffset = writer.bytes.size();
    writer.appendSpan(std::span<const RuntimeAssets::SubmeshRecord>(submeshRecords));
    writer.align(16);
    header.blobDataOffset = writer.bytes.size();
    for (const ImportedMesh& mesh : pack.meshes)
    {
        std::vector<std::byte> compressedVertexBytes;
        if (!RuntimeAssets::CompressBytes(
                RuntimeAssets::CompressionType::Lz4,
                std::as_bytes(std::span<const RuntimeAssets::MeshVertex>(mesh.vertices)),
                &compressedVertexBytes,
                error))
        {
            return false;
        }

        std::vector<std::byte> compressedIndexBytes;
        if (!RuntimeAssets::CompressBytes(
                RuntimeAssets::CompressionType::Lz4,
                std::as_bytes(std::span<const std::uint32_t>(mesh.indices)),
                &compressedIndexBytes,
                error))
        {
            return false;
        }

        RuntimeAssets::MeshBlobRecord blob{};
        blob.compressionType = static_cast<std::uint32_t>(RuntimeAssets::CompressionType::Lz4);

        writer.align(16);
        blob.vertexDataOffset = writer.bytes.size();
        blob.vertexDataCompressedSize = compressedVertexBytes.size();
        blob.vertexDataUncompressedSize = mesh.vertices.size() * sizeof(RuntimeAssets::MeshVertex);
        writer.appendBytes(compressedVertexBytes);

        writer.align(16);
        blob.indexDataOffset = writer.bytes.size();
        blob.indexDataCompressedSize = compressedIndexBytes.size();
        blob.indexDataUncompressedSize = mesh.indices.size() * sizeof(std::uint32_t);
        writer.appendBytes(compressedIndexBytes);

        meshBlobs.push_back(blob);
    }
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

    if (outMeshBlobs != nullptr)
    {
        *outMeshBlobs = std::move(meshBlobs);
    }
    *outFileSize = header.fileSize;
    return true;
}
