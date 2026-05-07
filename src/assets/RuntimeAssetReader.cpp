#include "RuntimeAssetReader.hpp"

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
    if (version != kFormatVersion)
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

    if (!CheckRegion<MeshRecord>(header.meshRecordOffset, header.meshCount, size, "mesh records", error) ||
        !CheckRegion<SubmeshRecord>(header.submeshRecordOffset, header.submeshCount, size, "submesh records", error) ||
        !CheckRegion<MeshVertex>(header.vertexDataOffset, header.vertexCount, size, "vertex data", error) ||
        !CheckRegion<std::uint32_t>(header.indexDataOffset, header.indexCount, size, "index data", error) ||
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

        if (!CheckByteRegion(texture.dataOffset, texture.dataSize, size, "texture blob", error))
        {
            return false;
        }

        if (texture.dataOffset < header.pixelDataOffset || texture.dataOffset >= header.stringTableOffset)
        {
            if (error != nullptr)
            {
                *error = "texture data offset is outside the pixel data region";
            }
            return false;
        }

        if (texture.dataSize < static_cast<std::uint64_t>(texture.width) * texture.height * 4u)
        {
            if (error != nullptr)
            {
                *error = "texture data size is smaller than RGBA8 pixels";
            }
            return false;
        }
    }

    return true;
}

bool ValidateAssetBin(const void* data, std::size_t size, std::string* error)
{
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
        !CheckRegion<std::uint32_t>(header.textureRefRecordOffset, header.textureRefCount, size, "texture ref records", error) ||
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

    return true;
}

bool LoadMeshBinFromSDL(const char* path, LoadedMeshBinView* out, std::string* error)
{
    out->bytes.clear();
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
    out->vertices = MakeSpan<MeshVertex>(out->bytes, out->header.vertexDataOffset, out->header.vertexCount);
    out->indices = MakeSpan<std::uint32_t>(out->bytes, out->header.indexDataOffset, out->header.indexCount);
    out->stringTable = MakeByteSpan(out->bytes, out->header.stringTableOffset, out->header.stringTableSize);
    return true;
}

bool LoadTexBinFromSDL(const char* path, LoadedTexBinView* out, std::string* error)
{
    out->bytes.clear();
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
    out->textures = MakeSpan<TextureRecord>(out->bytes, out->header.textureRecordOffset, out->header.textureCount);
    out->pixelData = MakeByteSpan(
        out->bytes,
        out->header.pixelDataOffset,
        out->header.stringTableOffset - out->header.pixelDataOffset);
    out->stringTable = MakeByteSpan(out->bytes, out->header.stringTableOffset, out->header.stringTableSize);
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

    out->header = ReadStruct<AssetBinHeader>(out->bytes.data());
    out->assets = MakeSpan<AssetRecord>(out->bytes, out->header.assetRecordOffset, out->header.assetCount);
    out->materials = MakeSpan<MaterialRecord>(out->bytes, out->header.materialRecordOffset, out->header.materialCount);
    out->meshRefs = MakeSpan<MeshRefRecord>(out->bytes, out->header.meshRefRecordOffset, out->header.meshRefCount);
    out->textureRefs = MakeSpan<std::uint32_t>(out->bytes, out->header.textureRefRecordOffset, out->header.textureRefCount);
    out->stringTable = MakeByteSpan(out->bytes, out->header.stringTableOffset, out->header.stringTableSize);
    return true;
}

} // namespace RuntimeAssets
