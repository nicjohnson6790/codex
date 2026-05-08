#include "TexBinWriter.hpp"

#include "../../src/assets/RuntimeAssetCompression.hpp"
#include "../../src/assets/RuntimeAssetFormat.hpp"

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

    void appendBytes(std::span<const std::byte> value)
    {
        bytes.insert(bytes.end(), value.begin(), value.end());
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

bool WriteTexBin(
    const ImportedPack& pack,
    const std::filesystem::path& outputPath,
    std::vector<RuntimeAssets::TextureBlobRecord>* outTextureBlobs,
    std::uint64_t* outFileSize,
    std::string* error)
{
    std::vector<RuntimeAssets::TextureRecord> textureRecords;
    std::vector<RuntimeAssets::TextureBlobRecord> textureBlobs;
    std::string stringTable;

    textureRecords.reserve(pack.textures.size());
    textureBlobs.reserve(pack.textures.size());
    for (const ImportedTexture& texture : pack.textures)
    {
        RuntimeAssets::TextureRecord record{};
        record.nameOffset = AppendString(texture.name, &stringTable);
        record.sourcePathOffset = AppendString(texture.sourcePath, &stringTable);
        record.width = texture.width;
        record.height = texture.height;
        record.mipCount = 1;
        record.format = static_cast<std::uint32_t>(texture.format);
        record.dataSize = texture.pixels.size();
        record.flags = texture.flags;
        textureRecords.push_back(record);
    }

    RuntimeAssets::TexBinHeader header{};
    header.magic = RuntimeAssets::kTexBinMagic;
    header.version = RuntimeAssets::kFormatVersion;
    header.flags = RuntimeAssets::kLittleEndianFlag;
    header.textureCount = static_cast<std::uint32_t>(textureRecords.size());

    ByteWriter writer;
    writer.appendValue(header);
    writer.align(8);
    header.textureRecordOffset = writer.bytes.size();
    writer.appendSpan(std::span<const RuntimeAssets::TextureRecord>(textureRecords));
    writer.align(16);
    header.pixelDataOffset = writer.bytes.size();

    for (std::size_t index = 0; index < pack.textures.size(); ++index)
    {
        std::vector<std::byte> compressedPixels;
        if (!RuntimeAssets::CompressBytes(
                RuntimeAssets::CompressionType::Lz4,
                std::span<const std::byte>(pack.textures[index].pixels),
                &compressedPixels,
                error))
        {
            return false;
        }

        writer.align(16);
        RuntimeAssets::TextureBlobRecord blob{};
        blob.dataOffset = writer.bytes.size();
        blob.dataCompressedSize = compressedPixels.size();
        blob.dataUncompressedSize = pack.textures[index].pixels.size();
        blob.compressionType = static_cast<std::uint32_t>(RuntimeAssets::CompressionType::Lz4);
        writer.appendBytes(compressedPixels);
        textureBlobs.push_back(blob);
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
    std::memcpy(
        writer.bytes.data() + header.textureRecordOffset,
        textureRecords.data(),
        textureRecords.size() * sizeof(RuntimeAssets::TextureRecord));

    if (!WriteAllBytes(outputPath, writer.bytes, error))
    {
        return false;
    }

    if (outTextureBlobs != nullptr)
    {
        *outTextureBlobs = std::move(textureBlobs);
    }
    *outFileSize = header.fileSize;
    return true;
}
