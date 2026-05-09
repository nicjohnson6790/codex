#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace RuntimeAssets
{

constexpr std::uint32_t kFormatVersion = 3;
constexpr std::uint32_t kLittleEndianFlag = 1u << 0;

constexpr std::uint32_t MakeMagic(char a, char b, char c, char d)
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8u) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16u) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24u);
}

constexpr std::uint32_t kMeshBinMagic = MakeMagic('M', 'S', 'H', 'B');
constexpr std::uint32_t kTexBinMagic = MakeMagic('T', 'E', 'X', 'B');
constexpr std::uint32_t kAssetBinMagic = MakeMagic('A', 'S', 'B', 'N');

enum class TextureFormat : std::uint32_t
{
    RGBA8_UNORM = 1,
    RGBA8_SRGB = 2,
    BC3_RGBA_UNORM = 3,
    BC5_RG_UNORM = 4,
    BC3_RGBA_SRGB = 5,
};

enum class TextureDimension : std::uint32_t
{
    Texture2D = 1,
    Texture2DArray = 2,
};

enum TextureFlags : std::uint32_t
{
    TextureFlagNone = 0,
    TextureFlagSrgb = 1u << 0,
};

enum MaterialFlags : std::uint32_t
{
    MaterialFlagNone = 0,
    MaterialFlagAlphaMasked = 1u << 0,
    MaterialFlagDoubleSided = 1u << 1,
    MaterialFlagSwayEnabled = 1u << 2,
    MaterialFlagSrgbBaseColor = 1u << 3,
};

enum class CompressionType : std::uint32_t
{
    None = 0,
    Lz4 = 1,
};

struct MeshBinHeader
{
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint32_t meshCount = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t submeshCount = 0;
    std::uint32_t reserved = 0;
    std::uint64_t meshRecordOffset = 0;
    std::uint64_t submeshRecordOffset = 0;
    std::uint64_t blobDataOffset = 0;
    std::uint64_t stringTableOffset = 0;
    std::uint64_t stringTableSize = 0;
    std::uint64_t fileSize = 0;
};
static_assert(sizeof(MeshBinHeader) == 80);

struct MeshRecord
{
    std::uint32_t nameOffset = 0;
    std::uint32_t firstSubmesh = 0;
    std::uint32_t submeshCount = 0;
    std::uint32_t firstVertex = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};
    std::array<float, 3> boundsSphereCenter{};
    float boundsSphereRadius = 0.0f;
};
static_assert(sizeof(MeshRecord) == 68);

struct SubmeshRecord
{
    std::uint32_t meshIndex = 0;
    std::uint32_t materialIndex = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t firstVertex = 0;
    std::uint32_t vertexCount = 0;
};
static_assert(sizeof(SubmeshRecord) == 24);

struct MeshVertex
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 4> tangent{};
    std::array<float, 2> uv0{};
};
static_assert(sizeof(MeshVertex) == 48);

struct TexBinHeader
{
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint32_t textureCount = 0;
    std::uint64_t textureRecordOffset = 0;
    std::uint64_t pixelDataOffset = 0;
    std::uint64_t stringTableOffset = 0;
    std::uint64_t stringTableSize = 0;
    std::uint64_t fileSize = 0;
};
static_assert(sizeof(TexBinHeader) == 56);

constexpr std::uint32_t TextureBytesPerPixel(TextureFormat format)
{
    switch (format)
    {
    case TextureFormat::RGBA8_UNORM:
    case TextureFormat::RGBA8_SRGB:
        return 4u;
    case TextureFormat::BC3_RGBA_UNORM:
    case TextureFormat::BC5_RG_UNORM:
    case TextureFormat::BC3_RGBA_SRGB:
        return 0u;
    }
    return 0u;
}

constexpr bool TextureFormatIsBlockCompressed(TextureFormat format)
{
    return format == TextureFormat::BC3_RGBA_UNORM ||
        format == TextureFormat::BC5_RG_UNORM ||
        format == TextureFormat::BC3_RGBA_SRGB;
}

constexpr std::uint32_t TextureBlockSizeBytes(TextureFormat format)
{
    switch (format)
    {
    case TextureFormat::BC3_RGBA_UNORM:
    case TextureFormat::BC5_RG_UNORM:
    case TextureFormat::BC3_RGBA_SRGB:
        return 16u;
    case TextureFormat::RGBA8_UNORM:
    case TextureFormat::RGBA8_SRGB:
        return 0u;
    }
    return 0u;
}

constexpr std::uint32_t TextureMipExtent(std::uint32_t baseExtent, std::uint32_t mipIndex)
{
    return mipIndex >= 31u ? 1u : std::max(baseExtent >> mipIndex, 1u);
}

constexpr std::uint64_t CalculateTextureMipByteSize(
    TextureFormat format,
    std::uint32_t width,
    std::uint32_t height)
{
    if (width == 0u || height == 0u)
    {
        return 0u;
    }

    if (TextureFormatIsBlockCompressed(format))
    {
        const std::uint64_t blockWidth = std::max<std::uint64_t>((static_cast<std::uint64_t>(width) + 3u) / 4u, 1u);
        const std::uint64_t blockHeight = std::max<std::uint64_t>((static_cast<std::uint64_t>(height) + 3u) / 4u, 1u);
        return blockWidth * blockHeight * TextureBlockSizeBytes(format);
    }

    return static_cast<std::uint64_t>(width) * height * TextureBytesPerPixel(format);
}

constexpr std::uint64_t CalculateTextureDataSize(
    TextureFormat format,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t layerCount,
    std::uint32_t mipCount)
{
    if (width == 0u || height == 0u || layerCount == 0u || mipCount == 0u)
    {
        return 0u;
    }

    std::uint64_t totalSize = 0u;
    for (std::uint32_t layerIndex = 0; layerIndex < layerCount; ++layerIndex)
    {
        (void)layerIndex;
        for (std::uint32_t mipIndex = 0; mipIndex < mipCount; ++mipIndex)
        {
            totalSize += CalculateTextureMipByteSize(
                format,
                TextureMipExtent(width, mipIndex),
                TextureMipExtent(height, mipIndex));
        }
    }
    return totalSize;
}

struct TextureRecord
{
    std::uint32_t nameOffset = 0;
    std::uint32_t sourcePathOffset = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t layerCount = 1;
    std::uint32_t mipCount = 0;
    std::uint32_t format = 0;
    std::uint32_t dimension = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
    std::uint64_t dataOffset = 0;
    std::uint64_t dataCompressedSize = 0;
    std::uint64_t dataUncompressedSize = 0;
};
static_assert(sizeof(TextureRecord) == 64);

struct MeshBlobRecord
{
    std::uint64_t vertexDataOffset = 0;
    std::uint64_t vertexDataCompressedSize = 0;
    std::uint64_t vertexDataUncompressedSize = 0;
    std::uint64_t indexDataOffset = 0;
    std::uint64_t indexDataCompressedSize = 0;
    std::uint64_t indexDataUncompressedSize = 0;
    std::uint32_t compressionType = 0;
    std::uint32_t reserved = 0;
};
static_assert(sizeof(MeshBlobRecord) == 56);

struct TextureBlobRecord
{
    std::uint64_t dataOffset = 0;
    std::uint64_t dataCompressedSize = 0;
    std::uint64_t dataUncompressedSize = 0;
    std::uint32_t compressionType = 0;
    std::uint32_t reserved = 0;
};
static_assert(sizeof(TextureBlobRecord) == 32);

struct AssetBinHeader
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
static_assert(sizeof(AssetBinHeader) == 112);

struct AssetRecord
{
    std::uint32_t nameOffset = 0;
    std::uint32_t firstMeshRef = 0;
    std::uint32_t meshRefCount = 0;
    std::uint32_t firstMaterial = 0;
    std::uint32_t materialCount = 0;
    std::uint32_t flags = 0;
    std::uint32_t imposterColorTextureIndex = 0;
    std::uint32_t imposterNormalTextureIndex = 0;
};
static_assert(sizeof(AssetRecord) == 32);

struct MeshRefRecord
{
    std::uint32_t meshIndex = 0;
    std::uint32_t lodIndex = 0;
    float maxDistanceMeters = 0.0f;
    std::uint32_t flags = 0;
};
static_assert(sizeof(MeshRefRecord) == 16);

struct MaterialRecord
{
    std::uint32_t nameOffset = 0;
    std::uint32_t baseColorTextureIndex = 0;
    std::uint32_t normalTextureIndex = 0;
    std::uint32_t roughnessTextureIndex = 0;
    std::uint32_t specularTextureIndex = 0;
    std::uint32_t aoTextureIndex = 0;
    std::uint32_t subsurfaceTextureIndex = 0;
    std::uint32_t displacementTextureIndex = 0;
    std::uint32_t flags = 0;
    float alphaCutoff = 0.0f;
};
static_assert(sizeof(MaterialRecord) == 40);

} // namespace RuntimeAssets
