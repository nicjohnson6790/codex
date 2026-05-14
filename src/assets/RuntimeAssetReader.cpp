#include "RuntimeAssetReader.hpp"
#include "RuntimeAssetCompression.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>
#include <type_traits>

namespace RuntimeAssets
{
namespace
{

constexpr std::uint32_t kPreviousFormatVersion = 3;

struct AssetBinHeaderV3
{
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint32_t meshBinPathOffset = 0;
    std::uint32_t texBinPathOffset = 0;
    std::uint32_t assetCount = 0;
    std::uint32_t materialCount = 0;
    std::uint32_t meshRefCount = 0;
    std::uint32_t meshBlobCount = 0;
    std::uint32_t textureBlobCount = 0;
    std::uint32_t reserved = 0;
    std::uint64_t assetRecordOffset = 0;
    std::uint64_t materialRecordOffset = 0;
    std::uint64_t meshRefRecordOffset = 0;
    std::uint64_t meshBlobRecordOffset = 0;
    std::uint64_t textureBlobRecordOffset = 0;
    std::uint64_t stringTableOffset = 0;
    std::uint64_t stringTableSize = 0;
    std::uint64_t fileSize = 0;
};
static_assert(sizeof(AssetBinHeaderV3) == 112);

template <typename T>
bool IsAligned(std::uint64_t value)
{
    return (value % alignof(T)) == 0;
}

template <typename T>
bool CheckRegion(
    std::uint64_t offset,
    std::uint64_t count,
    std::size_t fileSize,
    const char* label,
    std::string* error)
{
    static_assert(std::is_trivially_copyable_v<T>);
    const std::uint64_t elementSize = sizeof(T);
    if (count == 0)
    {
        if (offset > fileSize)
        {
            if (error != nullptr)
            {
                *error = std::string(label) + " offset is outside the file";
            }
            return false;
        }
        return true;
    }

    if (!IsAligned<T>(offset))
    {
        if (error != nullptr)
        {
            *error = std::string(label) + " offset is misaligned";
        }
        return false;
    }

    if (offset > fileSize)
    {
        if (error != nullptr)
        {
            *error = std::string(label) + " offset is outside the file";
        }
        return false;
    }

    const std::uint64_t byteCount = count * elementSize;
    if (count > (std::numeric_limits<std::uint64_t>::max() / elementSize) ||
        offset > static_cast<std::uint64_t>(fileSize) ||
        byteCount > (static_cast<std::uint64_t>(fileSize) - offset))
    {
        if (error != nullptr)
        {
            *error = std::string(label) + " range exceeds the file";
        }
        return false;
    }

    return true;
}

bool CheckByteRegion(
    std::uint64_t offset,
    std::uint64_t sizeBytes,
    std::size_t fileSize,
    const char* label,
    std::string* error)
{
    if (offset > fileSize || sizeBytes > (static_cast<std::uint64_t>(fileSize) - offset))
    {
        if (error != nullptr)
        {
            *error = std::string(label) + " range exceeds the file";
        }
        return false;
    }
    return true;
}

bool CheckBlobRegion(
    std::uint64_t offset,
    std::uint64_t sizeBytes,
    std::uint64_t regionBegin,
    std::uint64_t regionEnd,
    const char* label,
    std::string* error)
{
    if (offset < regionBegin || offset > regionEnd || sizeBytes > (regionEnd - offset))
    {
        if (error != nullptr)
        {
            *error = std::string(label) + " range exceeds the blob region";
        }
        return false;
    }
    return true;
}

bool StringOffsetInTable(std::uint32_t offset, std::span<const std::byte> table)
{
    if (offset >= table.size())
    {
        return false;
    }

    const char* begin = reinterpret_cast<const char*>(table.data() + offset);
    const std::size_t remaining = table.size() - offset;
    return std::memchr(begin, '\0', remaining) != nullptr;
}

template <typename T>
T ReadStruct(const void* data)
{
    T value{};
    std::memcpy(&value, data, sizeof(T));
    return value;
}

bool ReadFileBytes(const char* path, std::vector<std::byte>* out, std::string* error)
{
    SDL_IOStream* stream = SDL_IOFromFile(path, "rb");
    if (stream == nullptr)
    {
        if (error != nullptr)
        {
            *error = std::string("SDL_IOFromFile failed for ") + path + ": " + SDL_GetError();
        }
        return false;
    }

    const Sint64 fileSize = SDL_GetIOSize(stream);
    if (fileSize < 0)
    {
        if (error != nullptr)
        {
            *error = std::string("SDL_GetIOSize failed for ") + path + ": " + SDL_GetError();
        }
        SDL_CloseIO(stream);
        return false;
    }

    out->assign(static_cast<std::size_t>(fileSize), std::byte{});
    std::size_t totalRead = 0;
    while (totalRead < out->size())
    {
        const std::size_t readCount = SDL_ReadIO(
            stream,
            out->data() + totalRead,
            out->size() - totalRead);
        if (readCount == 0)
        {
            if (error != nullptr)
            {
                *error = std::string("SDL_ReadIO failed for ") + path + ": " + SDL_GetError();
            }
            SDL_CloseIO(stream);
            return false;
        }
        totalRead += readCount;
    }

    if (!SDL_CloseIO(stream))
    {
        if (error != nullptr)
        {
            *error = std::string("SDL_CloseIO failed for ") + path + ": " + SDL_GetError();
        }
        return false;
    }

    return true;
}

template <typename T>
std::span<const T> MakeSpan(const std::vector<std::byte>& bytes, std::uint64_t offset, std::uint32_t count)
{
    return std::span<const T>(
        reinterpret_cast<const T*>(bytes.data() + offset),
        static_cast<std::size_t>(count));
}

std::span<const std::byte> MakeByteSpan(const std::vector<std::byte>& bytes, std::uint64_t offset, std::uint64_t size)
{
    return std::span<const std::byte>(
        bytes.data() + offset,
        static_cast<std::size_t>(size));
}

bool ValidateCommonHeader(
    std::uint32_t expectedMagic,
    std::uint32_t version,
    std::uint32_t flags,
    std::uint64_t fileSizeField,
    std::size_t size,
    std::string* error)
{
    if (version != kFormatVersion && version != kPreviousFormatVersion)
    {
        if (error != nullptr)
        {
            *error = "unsupported runtime asset version";
        }
        return false;
    }

    if ((flags & kLittleEndianFlag) == 0)
    {
        if (error != nullptr)
        {
            *error = "runtime asset file is missing the little-endian flag";
        }
        return false;
    }

    if (fileSizeField != size)
    {
        if (error != nullptr)
        {
            *error = "fileSize field does not match the actual file size";
        }
        return false;
    }

    (void)expectedMagic;
    return true;
}

bool ValidateAssetBinV3(const void* data, std::size_t size, std::string* error)
{
    if (size < sizeof(AssetBinHeaderV3))
    {
        if (error != nullptr)
        {
            *error = "assetbin is smaller than AssetBinHeaderV3";
        }
        return false;
    }

    const AssetBinHeaderV3 header = ReadStruct<AssetBinHeaderV3>(data);
    if (header.magic != kAssetBinMagic)
    {
        if (error != nullptr)
        {
            *error = "assetbin magic mismatch";
        }
        return false;
    }

    if (!ValidateCommonHeader(kAssetBinMagic, header.version, header.flags, header.fileSize, size, error))
    {
        return false;
    }

    if (!CheckRegion<AssetRecord>(header.assetRecordOffset, header.assetCount, size, "asset records", error) ||
        !CheckRegion<MaterialRecord>(header.materialRecordOffset, header.materialCount, size, "material records", error) ||
        !CheckRegion<MeshRefRecord>(header.meshRefRecordOffset, header.meshRefCount, size, "mesh ref records", error) ||
        !CheckRegion<MeshBlobRecord>(header.meshBlobRecordOffset, header.meshBlobCount, size, "mesh blob records", error) ||
        !CheckRegion<TextureBlobRecord>(header.textureBlobRecordOffset, header.textureBlobCount, size, "texture blob records", error) ||
        !CheckByteRegion(header.stringTableOffset, header.stringTableSize, size, "string table", error))
    {
        return false;
    }

    const auto stringTable = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(data) + header.stringTableOffset,
        static_cast<std::size_t>(header.stringTableSize));
    if (!StringOffsetInTable(header.meshBinPathOffset, stringTable) ||
        !StringOffsetInTable(header.texBinPathOffset, stringTable))
    {
        if (error != nullptr)
        {
            *error = "assetbin path string offset is outside the string table";
        }
        return false;
    }

    const auto assets = std::span<const AssetRecord>(
        reinterpret_cast<const AssetRecord*>(reinterpret_cast<const std::byte*>(data) + header.assetRecordOffset),
        static_cast<std::size_t>(header.assetCount));
    const auto materials = std::span<const MaterialRecord>(
        reinterpret_cast<const MaterialRecord*>(reinterpret_cast<const std::byte*>(data) + header.materialRecordOffset),
        static_cast<std::size_t>(header.materialCount));
    const auto meshBlobs = std::span<const MeshBlobRecord>(
        reinterpret_cast<const MeshBlobRecord*>(reinterpret_cast<const std::byte*>(data) + header.meshBlobRecordOffset),
        static_cast<std::size_t>(header.meshBlobCount));
    const auto textureBlobs = std::span<const TextureBlobRecord>(
        reinterpret_cast<const TextureBlobRecord*>(reinterpret_cast<const std::byte*>(data) + header.textureBlobRecordOffset),
        static_cast<std::size_t>(header.textureBlobCount));

    for (const AssetRecord& asset : assets)
    {
        if (!StringOffsetInTable(asset.nameOffset, stringTable))
        {
            if (error != nullptr)
            {
                *error = "asset name offset is outside the string table";
            }
            return false;
        }
    }

    for (const MaterialRecord& material : materials)
    {
        if (!StringOffsetInTable(material.nameOffset, stringTable))
        {
            if (error != nullptr)
            {
                *error = "material name offset is outside the string table";
            }
            return false;
        }
    }

    for (const MeshBlobRecord& blob : meshBlobs)
    {
        if (blob.compressionType != static_cast<std::uint32_t>(CompressionType::None) &&
            blob.compressionType != static_cast<std::uint32_t>(CompressionType::Lz4))
        {
            if (error != nullptr)
            {
                *error = "mesh blob compression type is unsupported";
            }
            return false;
        }
    }

    for (const TextureBlobRecord& blob : textureBlobs)
    {
        if (blob.compressionType != static_cast<std::uint32_t>(CompressionType::None) &&
            blob.compressionType != static_cast<std::uint32_t>(CompressionType::Lz4))
        {
            if (error != nullptr)
            {
                *error = "texture blob compression type is unsupported";
            }
            return false;
        }
    }

    return true;
}

} // namespace

const char* LoadedMeshBinView::stringAt(std::uint32_t offset) const
{
    return reinterpret_cast<const char*>(stringTable.data() + offset);
}

const char* LoadedTexBinView::stringAt(std::uint32_t offset) const
{
    return reinterpret_cast<const char*>(stringTable.data() + offset);
}

const char* LoadedAssetBinView::stringAt(std::uint32_t offset) const
{
    return reinterpret_cast<const char*>(stringTable.data() + offset);
}

bool ValidateMeshBin(const void* data, std::size_t size, std::string* error)
{
    if (size < sizeof(MeshBinHeader))
    {
        if (error != nullptr)
        {
            *error = "meshbin is smaller than MeshBinHeader";
        }
        return false;
    }

    const MeshBinHeader header = ReadStruct<MeshBinHeader>(data);
    if (header.magic != kMeshBinMagic)
    {
        if (error != nullptr)
        {
            *error = "meshbin magic mismatch";
        }
        return false;
    }

    if (!ValidateCommonHeader(kMeshBinMagic, header.version, header.flags, header.fileSize, size, error))
    {
        return false;
    }

    if (header.stringTableOffset < header.blobDataOffset)
    {
        if (error != nullptr)
        {
            *error = "meshbin string table overlaps the blob data region";
        }
        return false;
    }

    if (!CheckRegion<MeshRecord>(header.meshRecordOffset, header.meshCount, size, "mesh records", error) ||
        !CheckRegion<SubmeshRecord>(header.submeshRecordOffset, header.submeshCount, size, "submesh records", error) ||
        !CheckByteRegion(header.blobDataOffset, header.stringTableOffset - header.blobDataOffset, size, "blob data", error) ||
        !CheckByteRegion(header.stringTableOffset, header.stringTableSize, size, "string table", error))
    {
        return false;
    }

    const auto stringTable = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(data) + header.stringTableOffset,
        static_cast<std::size_t>(header.stringTableSize));
    const auto meshes = std::span<const MeshRecord>(
        reinterpret_cast<const MeshRecord*>(reinterpret_cast<const std::byte*>(data) + header.meshRecordOffset),
        static_cast<std::size_t>(header.meshCount));
    const auto submeshes = std::span<const SubmeshRecord>(
        reinterpret_cast<const SubmeshRecord*>(reinterpret_cast<const std::byte*>(data) + header.submeshRecordOffset),
        static_cast<std::size_t>(header.submeshCount));

    for (std::size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex)
    {
        const MeshRecord& mesh = meshes[meshIndex];
        if (!StringOffsetInTable(mesh.nameOffset, stringTable))
        {
            if (error != nullptr)
            {
                *error = "mesh record name offset is outside the string table";
            }
            return false;
        }

        if (mesh.firstSubmesh > header.submeshCount ||
            mesh.submeshCount > (header.submeshCount - mesh.firstSubmesh))
        {
            if (error != nullptr)
            {
                *error = "mesh record submesh range is invalid";
            }
            return false;
        }

        if (mesh.firstVertex > header.vertexCount ||
            mesh.vertexCount > (header.vertexCount - mesh.firstVertex))
        {
            if (error != nullptr)
            {
                *error = "mesh record vertex range is invalid";
            }
            return false;
        }

        if (mesh.firstIndex > header.indexCount ||
            mesh.indexCount > (header.indexCount - mesh.firstIndex))
        {
            if (error != nullptr)
            {
                *error = "mesh record index range is invalid";
            }
            return false;
        }
    }

    for (std::size_t submeshIndex = 0; submeshIndex < submeshes.size(); ++submeshIndex)
    {
        const SubmeshRecord& submesh = submeshes[submeshIndex];
        if (submesh.meshIndex >= header.meshCount)
        {
            if (error != nullptr)
            {
                *error = "submesh meshIndex is out of range";
            }
            return false;
        }

        if (submesh.firstVertex > header.vertexCount ||
            submesh.vertexCount > (header.vertexCount - submesh.firstVertex))
        {
            if (error != nullptr)
            {
                *error = "submesh vertex range is invalid";
            }
            return false;
        }

        if (submesh.firstIndex > header.indexCount ||
            submesh.indexCount > (header.indexCount - submesh.firstIndex))
        {
            if (error != nullptr)
            {
                *error = "submesh index range is invalid";
            }
            return false;
        }
    }

    return true;
}

bool ValidateTexBin(const void* data, std::size_t size, std::string* error)
{
    if (size < sizeof(TexBinHeader))
    {
        if (error != nullptr)
        {
            *error = "texbin is smaller than TexBinHeader";
        }
        return false;
    }

    const TexBinHeader header = ReadStruct<TexBinHeader>(data);
    if (header.magic != kTexBinMagic)
    {
        if (error != nullptr)
        {
            *error = "texbin magic mismatch";
        }
        return false;
    }

    if (!ValidateCommonHeader(kTexBinMagic, header.version, header.flags, header.fileSize, size, error))
    {
        return false;
    }

    if (header.stringTableOffset < header.pixelDataOffset)
    {
        if (error != nullptr)
        {
            *error = "texbin string table overlaps the pixel data region";
        }
        return false;
    }

    if (!CheckRegion<TextureRecord>(header.textureRecordOffset, header.textureCount, size, "texture records", error) ||
        !CheckByteRegion(header.pixelDataOffset, header.stringTableOffset - header.pixelDataOffset, size, "pixel data", error) ||
        !CheckByteRegion(header.stringTableOffset, header.stringTableSize, size, "string table", error))
    {
        return false;
    }

    const auto stringTable = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(data) + header.stringTableOffset,
        static_cast<std::size_t>(header.stringTableSize));
    const auto textures = std::span<const TextureRecord>(
        reinterpret_cast<const TextureRecord*>(reinterpret_cast<const std::byte*>(data) + header.textureRecordOffset),
        static_cast<std::size_t>(header.textureCount));

    for (const TextureRecord& texture : textures)
    {
        if (!StringOffsetInTable(texture.nameOffset, stringTable) ||
            !StringOffsetInTable(texture.sourcePathOffset, stringTable))
        {
            if (error != nullptr)
            {
                *error = "texture string offset is outside the string table";
            }
            return false;
        }

        if (texture.mipCount == 0)
        {
            if (error != nullptr)
            {
                *error = "texture mipCount must be at least 1";
            }
            return false;
        }

        if (texture.layerCount == 0)
        {
            if (error != nullptr)
            {
                *error = "texture layerCount must be at least 1";
            }
            return false;
        }

        if (texture.dimension != static_cast<std::uint32_t>(TextureDimension::Texture2D) &&
            texture.dimension != static_cast<std::uint32_t>(TextureDimension::Texture2DArray))
        {
            if (error != nullptr)
            {
                *error = "texture dimension is unsupported";
            }
            return false;
        }

        const TextureFormat format = static_cast<TextureFormat>(texture.format);
        const std::uint64_t expectedDataSize = CalculateTextureDataSize(
            format,
            texture.width,
            texture.height,
            texture.layerCount,
            texture.mipCount);
        if (expectedDataSize == 0)
        {
            if (error != nullptr)
            {
                *error = "texture dimensions must be non-zero";
            }
            return false;
        }

        if (texture.dataUncompressedSize != expectedDataSize)
        {
            if (error != nullptr)
            {
                *error = "texture uncompressed size does not match its format and dimensions";
            }
            return false;
        }

        if (!CheckBlobRegion(
                texture.dataOffset,
                texture.dataCompressedSize,
                header.pixelDataOffset,
                header.stringTableOffset,
                "texture payload",
                error))
        {
            return false;
        }
    }

    return true;
}

bool ValidateAssetBin(const void* data, std::size_t size, std::string* error)
{
    if (size < sizeof(AssetBinHeaderV3))
    {
        if (error != nullptr)
        {
            *error = "assetbin is smaller than AssetBinHeader";
        }
        return false;
    }

    const std::uint32_t version = ReadStruct<AssetBinHeaderV3>(data).version;
    if (version == kPreviousFormatVersion)
    {
        return ValidateAssetBinV3(data, size, error);
    }

    if (size < sizeof(AssetBinHeader))
    {
        if (error != nullptr)
        {
            *error = "assetbin is smaller than AssetBinHeader";
        }
        return false;
    }

    const AssetBinHeader header = ReadStruct<AssetBinHeader>(data);
    if (header.magic != kAssetBinMagic)
    {
        if (error != nullptr)
        {
            *error = "assetbin magic mismatch";
        }
        return false;
    }

    if (!ValidateCommonHeader(kAssetBinMagic, header.version, header.flags, header.fileSize, size, error))
    {
        return false;
    }

    if (!CheckRegion<AssetRecord>(header.assetRecordOffset, header.assetCount, size, "asset records", error) ||
        !CheckRegion<MaterialRecord>(header.materialRecordOffset, header.materialCount, size, "material records", error) ||
        !CheckRegion<MeshRefRecord>(header.meshRefRecordOffset, header.meshRefCount, size, "mesh ref records", error) ||
        !CheckRegion<MeshBlobRecord>(header.meshBlobRecordOffset, header.meshBlobCount, size, "mesh blob records", error) ||
        !CheckRegion<TextureBlobRecord>(header.textureBlobRecordOffset, header.textureBlobCount, size, "texture blob records", error) ||
        !CheckRegion<FontAtlasRecord>(header.fontAtlasRecordOffset, header.fontAtlasCount, size, "font atlas records", error) ||
        !CheckRegion<FontGlyphRecord>(header.fontGlyphRecordOffset, header.fontGlyphCount, size, "font glyph records", error) ||
        !CheckByteRegion(header.stringTableOffset, header.stringTableSize, size, "string table", error))
    {
        return false;
    }

    const auto stringTable = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(data) + header.stringTableOffset,
        static_cast<std::size_t>(header.stringTableSize));
    const auto assets = std::span<const AssetRecord>(
        reinterpret_cast<const AssetRecord*>(reinterpret_cast<const std::byte*>(data) + header.assetRecordOffset),
        static_cast<std::size_t>(header.assetCount));
    const auto materials = std::span<const MaterialRecord>(
        reinterpret_cast<const MaterialRecord*>(reinterpret_cast<const std::byte*>(data) + header.materialRecordOffset),
        static_cast<std::size_t>(header.materialCount));
    const auto meshBlobs = std::span<const MeshBlobRecord>(
        reinterpret_cast<const MeshBlobRecord*>(reinterpret_cast<const std::byte*>(data) + header.meshBlobRecordOffset),
        static_cast<std::size_t>(header.meshBlobCount));
    const auto textureBlobs = std::span<const TextureBlobRecord>(
        reinterpret_cast<const TextureBlobRecord*>(reinterpret_cast<const std::byte*>(data) + header.textureBlobRecordOffset),
        static_cast<std::size_t>(header.textureBlobCount));
    const auto fontAtlases = std::span<const FontAtlasRecord>(
        reinterpret_cast<const FontAtlasRecord*>(reinterpret_cast<const std::byte*>(data) + header.fontAtlasRecordOffset),
        static_cast<std::size_t>(header.fontAtlasCount));

    if (!StringOffsetInTable(header.meshBinPathOffset, stringTable) ||
        !StringOffsetInTable(header.texBinPathOffset, stringTable))
    {
        if (error != nullptr)
        {
            *error = "assetbin path string offset is outside the string table";
        }
        return false;
    }

    for (const AssetRecord& asset : assets)
    {
        if (!StringOffsetInTable(asset.nameOffset, stringTable))
        {
            if (error != nullptr)
            {
                *error = "asset name offset is outside the string table";
            }
            return false;
        }

        if (asset.firstMeshRef > header.meshRefCount ||
            asset.meshRefCount > (header.meshRefCount - asset.firstMeshRef))
        {
            if (error != nullptr)
            {
                *error = "asset mesh ref range is invalid";
            }
            return false;
        }

        if (asset.firstMaterial > header.materialCount ||
            asset.materialCount > (header.materialCount - asset.firstMaterial))
        {
            if (error != nullptr)
            {
                *error = "asset material range is invalid";
            }
            return false;
        }

        const std::uint32_t imposterTextureIndices[] = {
            asset.imposterColorTextureIndex,
            asset.imposterNormalTextureIndex,
        };
        for (std::uint32_t textureIndex : imposterTextureIndices)
        {
            if (textureIndex != std::numeric_limits<std::uint32_t>::max() &&
                textureIndex >= header.textureBlobCount)
            {
                if (error != nullptr)
                {
                    *error = "asset imposter texture index points past assetbin.textureBlobCount";
                }
                return false;
            }
        }
    }

    for (const MaterialRecord& material : materials)
    {
        if (!StringOffsetInTable(material.nameOffset, stringTable))
        {
            if (error != nullptr)
            {
                *error = "material name offset is outside the string table";
            }
            return false;
        }
    }

    for (const MeshBlobRecord& blob : meshBlobs)
    {
        if (blob.compressionType != static_cast<std::uint32_t>(CompressionType::None) &&
            blob.compressionType != static_cast<std::uint32_t>(CompressionType::Lz4))
        {
            if (error != nullptr)
            {
                *error = "mesh blob compression type is unsupported";
            }
            return false;
        }
    }

    for (const TextureBlobRecord& blob : textureBlobs)
    {
        if (blob.compressionType != static_cast<std::uint32_t>(CompressionType::None) &&
            blob.compressionType != static_cast<std::uint32_t>(CompressionType::Lz4))
        {
            if (error != nullptr)
            {
                *error = "texture blob compression type is unsupported";
            }
            return false;
        }
    }

    for (const FontAtlasRecord& fontAtlas : fontAtlases)
    {
        if (!StringOffsetInTable(fontAtlas.nameOffset, stringTable))
        {
            if (error != nullptr)
            {
                *error = "font atlas name offset is outside the string table";
            }
            return false;
        }

        if (fontAtlas.textureIndex >= header.textureBlobCount)
        {
            if (error != nullptr)
            {
                *error = "font atlas texture index points past assetbin.textureBlobCount";
            }
            return false;
        }

        if (fontAtlas.firstGlyph > header.fontGlyphCount ||
            fontAtlas.glyphCount > (header.fontGlyphCount - fontAtlas.firstGlyph))
        {
            if (error != nullptr)
            {
                *error = "font atlas glyph range is invalid";
            }
            return false;
        }

        if (fontAtlas.glyphCount == 0 || fontAtlas.pixelSize <= 0.0f || fontAtlas.distanceRange <= 0.0f)
        {
            if (error != nullptr)
            {
                *error = "font atlas metrics are invalid";
            }
            return false;
        }
    }

    return true;
}

bool LoadAssetBinFromSDL(const char* path, LoadedAssetBinView* out, std::string* error)
{
    out->bytes.clear();
    if (!ReadFileBytes(path, &out->bytes, error))
    {
        return false;
    }
    if (!ValidateAssetBin(out->bytes.data(), out->bytes.size(), error))
    {
        out->bytes.clear();
        return false;
    }

    const AssetBinHeaderV3 headerV3 = ReadStruct<AssetBinHeaderV3>(out->bytes.data());
    if (headerV3.version == kPreviousFormatVersion)
    {
        out->header = {};
        out->header.magic = headerV3.magic;
        out->header.version = headerV3.version;
        out->header.flags = headerV3.flags;
        out->header.meshBinPathOffset = headerV3.meshBinPathOffset;
        out->header.texBinPathOffset = headerV3.texBinPathOffset;
        out->header.assetCount = headerV3.assetCount;
        out->header.materialCount = headerV3.materialCount;
        out->header.meshRefCount = headerV3.meshRefCount;
        out->header.meshBlobCount = headerV3.meshBlobCount;
        out->header.textureBlobCount = headerV3.textureBlobCount;
        out->header.assetRecordOffset = headerV3.assetRecordOffset;
        out->header.materialRecordOffset = headerV3.materialRecordOffset;
        out->header.meshRefRecordOffset = headerV3.meshRefRecordOffset;
        out->header.meshBlobRecordOffset = headerV3.meshBlobRecordOffset;
        out->header.textureBlobRecordOffset = headerV3.textureBlobRecordOffset;
        out->header.stringTableOffset = headerV3.stringTableOffset;
        out->header.stringTableSize = headerV3.stringTableSize;
        out->header.fileSize = headerV3.fileSize;
    }
    else
    {
        out->header = ReadStruct<AssetBinHeader>(out->bytes.data());
    }

    out->assets = MakeSpan<AssetRecord>(out->bytes, out->header.assetRecordOffset, out->header.assetCount);
    out->materials = MakeSpan<MaterialRecord>(out->bytes, out->header.materialRecordOffset, out->header.materialCount);
    out->meshRefs = MakeSpan<MeshRefRecord>(out->bytes, out->header.meshRefRecordOffset, out->header.meshRefCount);
    out->meshBlobs = MakeSpan<MeshBlobRecord>(out->bytes, out->header.meshBlobRecordOffset, out->header.meshBlobCount);
    out->textureBlobs = MakeSpan<TextureBlobRecord>(out->bytes, out->header.textureBlobRecordOffset, out->header.textureBlobCount);
    out->fontAtlases = MakeSpan<FontAtlasRecord>(out->bytes, out->header.fontAtlasRecordOffset, out->header.fontAtlasCount);
    out->fontGlyphs = MakeSpan<FontGlyphRecord>(out->bytes, out->header.fontGlyphRecordOffset, out->header.fontGlyphCount);
    out->stringTable = MakeByteSpan(out->bytes, out->header.stringTableOffset, out->header.stringTableSize);
    return true;
}

bool LoadMeshBinFromSDL(
    const char* path,
    const LoadedAssetBinView& assetBin,
    LoadedMeshBinView* out,
    std::string* error)
{
    out->bytes.clear();
    out->decompressedVertices.clear();
    out->decompressedIndices.clear();
    if (!ReadFileBytes(path, &out->bytes, error))
    {
        return false;
    }
    if (!ValidateMeshBin(out->bytes.data(), out->bytes.size(), error))
    {
        out->bytes.clear();
        return false;
    }

    out->header = ReadStruct<MeshBinHeader>(out->bytes.data());
    out->meshes = MakeSpan<MeshRecord>(out->bytes, out->header.meshRecordOffset, out->header.meshCount);
    out->submeshes = MakeSpan<SubmeshRecord>(out->bytes, out->header.submeshRecordOffset, out->header.submeshCount);
    out->stringTable = MakeByteSpan(out->bytes, out->header.stringTableOffset, out->header.stringTableSize);

    if (assetBin.meshBlobs.size() != out->meshes.size())
    {
        if (error != nullptr)
        {
            *error = "assetbin mesh blob count does not match meshbin mesh count";
        }
        out->bytes.clear();
        return false;
    }

    out->decompressedVertices.resize(out->header.vertexCount);
    out->decompressedIndices.resize(out->header.indexCount);
    const std::uint64_t blobRegionEnd = out->header.stringTableOffset;

    for (std::size_t meshIndex = 0; meshIndex < out->meshes.size(); ++meshIndex)
    {
        const MeshRecord& mesh = out->meshes[meshIndex];
        const MeshBlobRecord& blob = assetBin.meshBlobs[meshIndex];

        if (!CheckBlobRegion(
                blob.vertexDataOffset,
                blob.vertexDataCompressedSize,
                out->header.blobDataOffset,
                blobRegionEnd,
                "mesh vertex blob",
                error) ||
            !CheckBlobRegion(
                blob.indexDataOffset,
                blob.indexDataCompressedSize,
                out->header.blobDataOffset,
                blobRegionEnd,
                "mesh index blob",
                error))
        {
            out->bytes.clear();
            out->decompressedVertices.clear();
            out->decompressedIndices.clear();
            return false;
        }

        if (blob.vertexDataUncompressedSize != static_cast<std::uint64_t>(mesh.vertexCount) * sizeof(MeshVertex) ||
            blob.indexDataUncompressedSize != static_cast<std::uint64_t>(mesh.indexCount) * sizeof(std::uint32_t))
        {
            if (error != nullptr)
            {
                *error = "mesh blob uncompressed sizes do not match mesh record counts";
            }
            out->bytes.clear();
            out->decompressedVertices.clear();
            out->decompressedIndices.clear();
            return false;
        }

        std::vector<std::byte> decompressedVertexBytes;
        if (!DecompressBytes(
                static_cast<CompressionType>(blob.compressionType),
                MakeByteSpan(out->bytes, blob.vertexDataOffset, blob.vertexDataCompressedSize),
                static_cast<std::size_t>(blob.vertexDataUncompressedSize),
                &decompressedVertexBytes,
                error))
        {
            out->bytes.clear();
            out->decompressedVertices.clear();
            out->decompressedIndices.clear();
            return false;
        }

        std::vector<std::byte> decompressedIndexBytes;
        if (!DecompressBytes(
                static_cast<CompressionType>(blob.compressionType),
                MakeByteSpan(out->bytes, blob.indexDataOffset, blob.indexDataCompressedSize),
                static_cast<std::size_t>(blob.indexDataUncompressedSize),
                &decompressedIndexBytes,
                error))
        {
            out->bytes.clear();
            out->decompressedVertices.clear();
            out->decompressedIndices.clear();
            return false;
        }

        std::memcpy(
            out->decompressedVertices.data() + mesh.firstVertex,
            decompressedVertexBytes.data(),
            decompressedVertexBytes.size());
        std::memcpy(
            out->decompressedIndices.data() + mesh.firstIndex,
            decompressedIndexBytes.data(),
            decompressedIndexBytes.size());
    }

    out->vertices = std::span<const MeshVertex>(out->decompressedVertices.data(), out->decompressedVertices.size());
    out->indices = std::span<const std::uint32_t>(out->decompressedIndices.data(), out->decompressedIndices.size());
    return true;
}

bool LoadTexBinFromSDL(
    const char* path,
    const LoadedAssetBinView& assetBin,
    LoadedTexBinView* out,
    std::string* error)
{
    out->bytes.clear();
    out->resolvedTextures.clear();
    out->decompressedPixelData.clear();
    if (!ReadFileBytes(path, &out->bytes, error))
    {
        return false;
    }
    if (!ValidateTexBin(out->bytes.data(), out->bytes.size(), error))
    {
        out->bytes.clear();
        return false;
    }

    out->header = ReadStruct<TexBinHeader>(out->bytes.data());
    const std::span<const TextureRecord> sourceTextures =
        MakeSpan<TextureRecord>(out->bytes, out->header.textureRecordOffset, out->header.textureCount);
    out->stringTable = MakeByteSpan(out->bytes, out->header.stringTableOffset, out->header.stringTableSize);

    if (assetBin.textureBlobs.size() != sourceTextures.size())
    {
        if (error != nullptr)
        {
            *error = "assetbin texture blob count does not match texbin texture count";
        }
        out->bytes.clear();
        return false;
    }

    out->resolvedTextures.assign(sourceTextures.begin(), sourceTextures.end());
    const std::uint64_t blobRegionEnd = out->header.stringTableOffset;

    for (std::size_t textureIndex = 0; textureIndex < out->resolvedTextures.size(); ++textureIndex)
    {
        TextureRecord& texture = out->resolvedTextures[textureIndex];
        const TextureBlobRecord& blob = assetBin.textureBlobs[textureIndex];
        const std::uint64_t expectedSize = CalculateTextureDataSize(
            static_cast<TextureFormat>(texture.format),
            texture.width,
            texture.height,
            texture.layerCount,
            texture.mipCount);

        if (!CheckBlobRegion(
                blob.dataOffset,
                blob.dataCompressedSize,
                out->header.pixelDataOffset,
                blobRegionEnd,
                "texture blob",
                error))
        {
            out->bytes.clear();
            out->resolvedTextures.clear();
            out->decompressedPixelData.clear();
            return false;
        }

        if (blob.dataUncompressedSize != expectedSize ||
            texture.dataUncompressedSize != expectedSize)
        {
            if (error != nullptr)
            {
                *error = "texture blob uncompressed size does not match texture dimensions";
            }
            out->bytes.clear();
            out->resolvedTextures.clear();
            out->decompressedPixelData.clear();
            return false;
        }

        if (texture.dataCompressedSize != blob.dataCompressedSize ||
            texture.dataOffset != blob.dataOffset)
        {
            if (error != nullptr)
            {
                *error = "texbin texture payload metadata does not match assetbin texture blob metadata";
            }
            out->bytes.clear();
            out->resolvedTextures.clear();
            out->decompressedPixelData.clear();
            return false;
        }

        std::vector<std::byte> decompressedTextureBytes;
        if (!DecompressBytes(
                static_cast<CompressionType>(blob.compressionType),
                MakeByteSpan(out->bytes, blob.dataOffset, blob.dataCompressedSize),
                static_cast<std::size_t>(blob.dataUncompressedSize),
                &decompressedTextureBytes,
                error))
        {
            out->bytes.clear();
            out->resolvedTextures.clear();
            out->decompressedPixelData.clear();
            return false;
        }

        texture.dataOffset = out->decompressedPixelData.size();
        texture.dataCompressedSize = blob.dataCompressedSize;
        texture.dataUncompressedSize = decompressedTextureBytes.size();
        out->decompressedPixelData.insert(
            out->decompressedPixelData.end(),
            decompressedTextureBytes.begin(),
            decompressedTextureBytes.end());
    }

    out->textures = std::span<const TextureRecord>(out->resolvedTextures.data(), out->resolvedTextures.size());
    out->pixelData = std::span<const std::byte>(out->decompressedPixelData.data(), out->decompressedPixelData.size());
    return true;
}

} // namespace RuntimeAssets
