#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace RuntimeAssets
{

constexpr std::uint32_t kFormatVersion = 1;
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
    std::uint64_t vertexDataOffset = 0;
    std::uint64_t indexDataOffset = 0;
    std::uint64_t stringTableOffset = 0;
    std::uint64_t stringTableSize = 0;
    std::uint64_t fileSize = 0;
};
static_assert(sizeof(MeshBinHeader) == 88);

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

struct TextureRecord
{
    std::uint32_t nameOffset = 0;
    std::uint32_t sourcePathOffset = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipCount = 0;
    std::uint32_t format = 0;
    std::uint64_t dataOffset = 0;
    std::uint64_t dataSize = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
};
static_assert(sizeof(TextureRecord) == 48);

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
    std::uint32_t textureRefCount = 0;
    std::uint32_t reserved = 0;
    std::uint64_t assetRecordOffset = 0;
    std::uint64_t materialRecordOffset = 0;
    std::uint64_t meshRefRecordOffset = 0;
    std::uint64_t textureRefRecordOffset = 0;
    std::uint64_t stringTableOffset = 0;
    std::uint64_t stringTableSize = 0;
    std::uint64_t fileSize = 0;
};
static_assert(sizeof(AssetBinHeader) == 96);

struct AssetRecord
{
    std::uint32_t nameOffset = 0;
    std::uint32_t firstMeshRef = 0;
    std::uint32_t meshRefCount = 0;
    std::uint32_t firstMaterial = 0;
    std::uint32_t materialCount = 0;
    std::uint32_t flags = 0;
};
static_assert(sizeof(AssetRecord) == 24);

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
