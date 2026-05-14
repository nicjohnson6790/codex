#include "PineTreePackConverter.hpp"

#include "AssetBinWriter.hpp"
#include "FbxImport.hpp"
#include "FontMsdfConverter.hpp"
#include "MeshBinWriter.hpp"
#include "PineImposterGenerator.hpp"
#include "TexBinWriter.hpp"
#include "TextureImport.hpp"
#include "../../src/assets/RuntimeAssetReader.hpp"

#include <SDL3/SDL.h>

#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace
{

void RebuildMaterialLayout(ImportedPack* pack)
{
    std::vector<ImportedMaterial> rebuiltMaterials;
    rebuiltMaterials.reserve(pack->materials.size());

    for (ImportedAsset& asset : pack->assets)
    {
        const std::uint32_t firstMaterial = static_cast<std::uint32_t>(rebuiltMaterials.size());
        std::unordered_map<std::uint32_t, std::uint32_t> remap;

        for (std::uint32_t meshIndex : asset.meshIndices)
        {
            ImportedMesh& mesh = pack->meshes[meshIndex];
            const auto existing = remap.find(mesh.materialIndex);
            if (existing != remap.end())
            {
                mesh.materialIndex = existing->second;
                continue;
            }

            const std::uint32_t newIndex = static_cast<std::uint32_t>(rebuiltMaterials.size());
            remap.emplace(mesh.materialIndex, newIndex);
            rebuiltMaterials.push_back(pack->materials[mesh.materialIndex]);
            mesh.materialIndex = newIndex;
        }

        asset.materialIndices.clear();
        for (std::uint32_t materialIndex = firstMaterial;
            materialIndex < static_cast<std::uint32_t>(rebuiltMaterials.size());
            ++materialIndex)
        {
            asset.materialIndices.push_back(materialIndex);
        }
    }

    pack->materials = std::move(rebuiltMaterials);
}

bool ValidateCrossReferences(
    const RuntimeAssets::LoadedMeshBinView* meshBin,
    const RuntimeAssets::LoadedTexBinView& texBin,
    const RuntimeAssets::LoadedAssetBinView& assetBin,
    bool requireImposterMetadata,
    std::string* error)
{
    const std::size_t meshCount = meshBin != nullptr ? meshBin->meshes.size() : 0u;
    if (assetBin.meshBlobs.size() != meshCount)
    {
        if (error != nullptr)
        {
            *error = "assetbin mesh blob count does not match meshbin mesh count";
        }
        return false;
    }

    if (assetBin.textureBlobs.size() != texBin.textures.size())
    {
        if (error != nullptr)
        {
            *error = "assetbin texture blob count does not match texbin texture count";
        }
        return false;
    }

    for (const RuntimeAssets::FontAtlasRecord& fontAtlas : assetBin.fontAtlases)
    {
        if (fontAtlas.textureIndex >= texBin.textures.size())
        {
            if (error != nullptr)
            {
                *error = "assetbin font atlas texture index points past texbin.textureCount";
            }
            return false;
        }

        const RuntimeAssets::TextureRecord& texture = texBin.textures[fontAtlas.textureIndex];
        if (texture.dimension != static_cast<std::uint32_t>(RuntimeAssets::TextureDimension::Texture2D) ||
            texture.format != static_cast<std::uint32_t>(RuntimeAssets::TextureFormat::RGBA8_UNORM) ||
            texture.mipCount != 1u)
        {
            if (error != nullptr)
            {
                *error = "font atlas texture metadata mismatch";
            }
            return false;
        }
    }

    for (const RuntimeAssets::MeshRefRecord& meshRef : assetBin.meshRefs)
    {
        if (meshRef.meshIndex >= meshCount)
        {
            if (error != nullptr)
            {
                *error = "assetbin meshRef points past meshbin.meshCount";
            }
            return false;
        }
    }

    const std::uint32_t kMissing = std::numeric_limits<std::uint32_t>::max();
    for (const RuntimeAssets::MaterialRecord& material : assetBin.materials)
    {
        const std::uint32_t indices[] = {
            material.baseColorTextureIndex,
            material.normalTextureIndex,
            material.roughnessTextureIndex,
            material.specularTextureIndex,
            material.aoTextureIndex,
            material.subsurfaceTextureIndex,
            material.displacementTextureIndex,
        };
        for (std::uint32_t textureIndex : indices)
        {
            if (textureIndex != kMissing && textureIndex >= texBin.textures.size())
            {
                if (error != nullptr)
                {
                    *error = "assetbin material texture index points past texbin.textureCount";
                }
                return false;
            }
        }
    }

    if (requireImposterMetadata)
    {
        for (const RuntimeAssets::AssetRecord& asset : assetBin.assets)
        {
            if (asset.imposterColorTextureIndex == kMissing ||
                asset.imposterNormalTextureIndex == kMissing)
            {
                if (error != nullptr)
                {
                    *error = "assetbin imposter metadata is missing";
                }
                return false;
            }

            if (asset.imposterColorTextureIndex >= texBin.textures.size() ||
                asset.imposterNormalTextureIndex >= texBin.textures.size())
            {
                if (error != nullptr)
                {
                    *error = "assetbin imposter metadata references missing texture records";
                }
                return false;
            }

            const RuntimeAssets::TextureRecord& colorTexture = texBin.textures[asset.imposterColorTextureIndex];
            const RuntimeAssets::TextureRecord& normalTexture = texBin.textures[asset.imposterNormalTextureIndex];
            const std::uint32_t expectedMipCount = [&]() {
                std::uint32_t mipCount = 1u;
                std::uint32_t extent = 512u;
                while (extent > 1u)
                {
                    extent = std::max(extent / 2u, 1u);
                    ++mipCount;
                }
                return mipCount;
            }();

            auto validateImposterTexture = [&](const RuntimeAssets::TextureRecord& texture,
                                                RuntimeAssets::TextureFormat expectedFormat,
                                                const char* label) -> bool {
                if (texture.dimension != static_cast<std::uint32_t>(RuntimeAssets::TextureDimension::Texture2DArray) ||
                    texture.width != 512u ||
                    texture.height != 512u ||
                    texture.layerCount != 32u ||
                    texture.mipCount != expectedMipCount ||
                    texture.format != static_cast<std::uint32_t>(expectedFormat))
                {
                    if (error != nullptr)
                    {
                        *error = std::string("imposter texture metadata mismatch for ") + label;
                    }
                    return false;
                }
                return true;
            };

            if (!validateImposterTexture(
                    colorTexture,
                    RuntimeAssets::TextureFormat::BC3_RGBA_UNORM,
                    "color/alpha") ||
                !validateImposterTexture(
                    normalTexture,
                    RuntimeAssets::TextureFormat::BC5_RG_UNORM,
                    "normal"))
            {
                return false;
            }
        }
    }

    if (meshBin != nullptr)
    {
        for (const RuntimeAssets::SubmeshRecord& submesh : meshBin->submeshes)
        {
            if (submesh.materialIndex >= assetBin.materials.size())
            {
                if (error != nullptr)
                {
                    *error = "meshbin submesh materialIndex points past assetbin.materialCount";
                }
                return false;
            }
        }
    }

    return true;
}

} // namespace

bool PineTreePackConverter::run(const ConverterConfig& config, ConversionSummary* outSummary, std::string* error)
{
    std::cout << "[converter] Begin pack '" << config.packName << "'\n";

    ImportedPack pack;
    std::size_t fbxFileCount = 0;
    TextureImportOptions textureOptions{};
    switch (config.packKind)
    {
    case ConverterConfig::PackKind::Skybox:
        textureOptions = TextureImportOptions{ .allowTga = false, .allowPng = true, .forceSrgb = true, .resizeSquare = 0u };
        break;
    case ConverterConfig::PackKind::Pbr:
        textureOptions = TextureImportOptions{
            .allowTga = false,
            .allowPng = true,
            .forceSrgb = false,
            .resizeSquare = 1024u,
            .compressPbrToBc = true,
        };
        break;
    case ConverterConfig::PackKind::Font:
        break;
    case ConverterConfig::PackKind::PineTree:
        textureOptions = TextureImportOptions{ .allowTga = true, .allowPng = false, .forceSrgb = false, .resizeSquare = 1024u };
        break;
    }

    if (config.packKind == ConverterConfig::PackKind::Font)
    {
        std::cout << "[converter] Generating font MSDF atlas\n";
        if (!GenerateFontMsdfAtlas(config, &pack, error))
        {
            return false;
        }
        std::cout << "[converter] Font atlas generated: " << pack.textures.size()
                  << " texture(s), " << pack.fontAtlases.size() << " atlas record(s)\n";
    }
    else if (!ImportTextureFolder(config.textureRoot, textureOptions, &pack, error))
    {
        return false;
    }
    else
    {
        std::cout << "[converter] Imported textures: " << pack.textures.size() << '\n';
    }
    if (config.packKind == ConverterConfig::PackKind::PineTree &&
        !ImportFbxFolder(config.fbxRoot, &pack, &fbxFileCount, error))
    {
        return false;
    }
    if (config.packKind == ConverterConfig::PackKind::PineTree)
    {
        std::cout << "[converter] Imported FBX files: " << fbxFileCount
                  << ", meshes: " << pack.meshes.size()
                  << ", assets: " << pack.assets.size() << '\n';
    }
    if (config.packKind == ConverterConfig::PackKind::PineTree)
    {
        std::cout << "[converter] Rebuilding material layout\n";
        RebuildMaterialLayout(&pack);
        std::cout << "[converter] Generating pine imposters\n";
        if (!GeneratePineImposters(&pack, error))
        {
            return false;
        }
        std::cout << "[converter] Pine imposters generated; total textures: " << pack.textures.size() << '\n';
    }

    std::filesystem::create_directories(config.outputRoot);
    const std::filesystem::path meshBinPath = config.outputRoot / (config.packName + ".meshbin");
    const std::filesystem::path texBinPath = config.outputRoot / (config.packName + ".texbin");
    const std::filesystem::path assetBinPath = config.outputRoot / (config.packName + ".assetbin");

    std::uint64_t meshBinSize = 0;
    std::uint64_t texBinSize = 0;
    std::uint64_t assetBinSize = 0;
    std::vector<RuntimeAssets::MeshBlobRecord> meshBlobs;
    std::vector<RuntimeAssets::TextureBlobRecord> textureBlobs;
    if (config.packKind == ConverterConfig::PackKind::PineTree)
    {
        std::cout << "[converter] Writing meshbin: " << meshBinPath.string() << '\n';
        if (!WriteMeshBin(pack, meshBinPath, &meshBlobs, &meshBinSize, error))
        {
            return false;
        }
        std::cout << "[converter] Meshbin bytes: " << meshBinSize << '\n';
    }

    std::cout << "[converter] Writing texbin: " << texBinPath.string() << '\n';
    if (!WriteTexBin(pack, texBinPath, &textureBlobs, &texBinSize, error))
    {
        return false;
    }
    std::cout << "[converter] Texbin bytes: " << texBinSize << '\n';

    std::cout << "[converter] Writing assetbin: " << assetBinPath.string() << '\n';
    if (!WriteAssetBin(
            pack,
            meshBlobs,
            textureBlobs,
            config.packKind == ConverterConfig::PackKind::PineTree ? (config.packName + ".meshbin") : "",
            config.packName + ".texbin",
            assetBinPath,
            &assetBinSize,
            error))
    {
        return false;
    }
    std::cout << "[converter] Assetbin bytes: " << assetBinSize << '\n';

    RuntimeAssets::LoadedMeshBinView loadedMesh;
    RuntimeAssets::LoadedTexBinView loadedTex;
    RuntimeAssets::LoadedAssetBinView loadedAsset;
    std::cout << "[converter] Validating generated runtime bins\n";
    if (!RuntimeAssets::LoadAssetBinFromSDL(assetBinPath.string().c_str(), &loadedAsset, error))
    {
        return false;
    }
    if (config.packKind == ConverterConfig::PackKind::PineTree &&
        !RuntimeAssets::LoadMeshBinFromSDL(meshBinPath.string().c_str(), loadedAsset, &loadedMesh, error))
    {
        return false;
    }
    if (!RuntimeAssets::LoadTexBinFromSDL(texBinPath.string().c_str(), loadedAsset, &loadedTex, error))
    {
        return false;
    }

    if (!ValidateCrossReferences(
            config.packKind == ConverterConfig::PackKind::PineTree ? &loadedMesh : nullptr,
            loadedTex,
            loadedAsset,
            config.packKind == ConverterConfig::PackKind::PineTree,
            error))
    {
        return false;
    }

    std::cout << "[converter] Validation complete\n";

    if (outSummary != nullptr)
    {
        outSummary->fbxFileCount = fbxFileCount;
        outSummary->meshCount = pack.meshes.size();
        for (const ImportedMesh& mesh : pack.meshes)
        {
            outSummary->vertexCount += mesh.vertices.size();
            outSummary->indexCount += mesh.indices.size();
        }
        outSummary->textureCount = pack.textures.size();
        outSummary->materialCount = pack.materials.size();
        outSummary->meshBinSize = meshBinSize;
        outSummary->texBinSize = texBinSize;
        outSummary->assetBinSize = assetBinSize;
    }

    return true;
}
