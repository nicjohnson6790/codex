#pragma once

#include "../../src/assets/RuntimeAssetFormat.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

struct ConverterConfig
{
    enum class PackKind
    {
        PineTree,
        Skybox,
        Pbr,
    };

    std::filesystem::path sourceRoot;
    std::filesystem::path fbxRoot;
    std::filesystem::path textureRoot;
    std::filesystem::path outputRoot;
    std::string packName;
    PackKind packKind = PackKind::PineTree;
};

struct TextureImportOptions
{
    bool allowTga = true;
    bool allowPng = false;
    bool forceSrgb = false;
    std::uint32_t resizeSquare = 0;
    bool compressPbrToBc = false;
};

struct ImportedMesh
{
    std::string sourceFile;
    std::string meshName;
    std::string assetName;
    std::uint32_t materialIndex = 0;
    std::uint32_t lodIndex = 0;
    float lodMaxDistance = 100.0f;
    std::vector<RuntimeAssets::MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};
    std::array<float, 3> boundsCenter{};
    float boundsRadius = 0.0f;
};

struct ImportedMaterial
{
    std::string sourceName;
    std::string normalizedName;
    RuntimeAssets::MaterialRecord record{};
};

struct ImportedTexture
{
    std::string name;
    std::string normalizedBasename;
    std::string sourcePath;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t layerCount = 1;
    std::uint32_t mipCount = 1;
    RuntimeAssets::TextureFormat format = RuntimeAssets::TextureFormat::RGBA8_UNORM;
    RuntimeAssets::TextureDimension dimension = RuntimeAssets::TextureDimension::Texture2D;
    std::uint32_t flags = 0;
    std::vector<std::byte> payload;
};

struct ImportedAsset
{
    std::string name;
    std::vector<std::uint32_t> meshIndices;
    std::vector<std::uint32_t> materialIndices;
    std::uint32_t imposterColorTextureIndex = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t imposterNormalTextureIndex = std::numeric_limits<std::uint32_t>::max();
};

struct ImportedPack
{
    std::vector<ImportedMesh> meshes;
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedTexture> textures;
    std::vector<ImportedAsset> assets;
    std::unordered_map<std::string, std::uint32_t> textureIndexByBasename;
};

struct ConversionSummary
{
    std::size_t fbxFileCount = 0;
    std::size_t meshCount = 0;
    std::size_t vertexCount = 0;
    std::size_t indexCount = 0;
    std::size_t textureCount = 0;
    std::size_t materialCount = 0;
    std::uint64_t meshBinSize = 0;
    std::uint64_t texBinSize = 0;
    std::uint64_t assetBinSize = 0;
};

class PineTreePackConverter
{
public:
    bool run(const ConverterConfig& config, ConversionSummary* outSummary, std::string* error);
};
