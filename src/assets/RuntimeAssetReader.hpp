#pragma once

#include "RuntimeAssetFormat.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace RuntimeAssets
{

struct LoadedMeshBinView
{
    std::vector<std::byte> bytes;
    MeshBinHeader header{};
    std::span<const MeshRecord> meshes;
    std::span<const SubmeshRecord> submeshes;
    std::vector<MeshVertex> decompressedVertices;
    std::vector<std::uint32_t> decompressedIndices;
    std::span<const MeshVertex> vertices;
    std::span<const std::uint32_t> indices;
    std::span<const std::byte> stringTable;

    [[nodiscard]] const char* stringAt(std::uint32_t offset) const;
};

struct LoadedTexBinView
{
    std::vector<std::byte> bytes;
    TexBinHeader header{};
    std::vector<TextureRecord> resolvedTextures;
    std::span<const TextureRecord> textures;
    std::vector<std::byte> decompressedPixelData;
    std::span<const std::byte> pixelData;
    std::span<const std::byte> stringTable;

    [[nodiscard]] const char* stringAt(std::uint32_t offset) const;
};

struct LoadedAssetBinView
{
    std::vector<std::byte> bytes;
    AssetBinHeader header{};
    std::span<const AssetRecord> assets;
    std::span<const MaterialRecord> materials;
    std::span<const MeshRefRecord> meshRefs;
    std::span<const MeshBlobRecord> meshBlobs;
    std::span<const TextureBlobRecord> textureBlobs;
    std::span<const std::byte> stringTable;

    [[nodiscard]] const char* stringAt(std::uint32_t offset) const;
};

bool ValidateMeshBin(const void* data, std::size_t size, std::string* error);
bool ValidateTexBin(const void* data, std::size_t size, std::string* error);
bool ValidateAssetBin(const void* data, std::size_t size, std::string* error);

bool LoadAssetBinFromSDL(const char* path, LoadedAssetBinView* out, std::string* error);
bool LoadMeshBinFromSDL(
    const char* path,
    const LoadedAssetBinView& assetBin,
    LoadedMeshBinView* out,
    std::string* error);
bool LoadTexBinFromSDL(
    const char* path,
    const LoadedAssetBinView& assetBin,
    LoadedTexBinView* out,
    std::string* error);

} // namespace RuntimeAssets
