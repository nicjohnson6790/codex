#include "PineTreePackConverter.hpp"

#include "AssetBinWriter.hpp"
#include "FbxImport.hpp"
#include "MeshBinWriter.hpp"
#include "PineImposterGenerator.hpp"
#include "TexBinWriter.hpp"
#include "TextureImport.hpp"
#include "../../src/assets/RuntimeAssetReader.hpp"

#include <SDL3/SDL.h>

#include <filesystem>
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
    ImportedPack pack;
    std::size_t fbxFileCount = 0;
    const TextureImportOptions textureOptions =
        config.packKind == ConverterConfig::PackKind::Skybox
        ? TextureImportOptions{ .allowTga = false, .allowPng = true, .forceSrgb = true, .resizeSquare = 0u }
        : TextureImportOptions{ .allowTga = true, .allowPng = false, .forceSrgb = false, .resizeSquare = 1024u };
    if (!ImportTextureFolder(config.textureRoot, textureOptions, &pack, error))
    {
        return false;
    }
    if (config.packKind == ConverterConfig::PackKind::PineTree &&
        !ImportFbxFolder(config.fbxRoot, &pack, &fbxFileCount, error))
    {
        return false;
    }
    if (config.packKind == ConverterConfig::PackKind::PineTree)
    {
        RebuildMaterialLayout(&pack);
        if (!GeneratePineImposters(&pack, error))
        {
            return false;
        }
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
    if (config.packKind == ConverterConfig::PackKind::PineTree &&
        !WriteMeshBin(pack, meshBinPath, &meshBlobs, &meshBinSize, error))
    {
        return false;
    }
    if (!WriteTexBin(pack, texBinPath, &textureBlobs, &texBinSize, error) ||
        !WriteAssetBin(
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

    RuntimeAssets::LoadedMeshBinView loadedMesh;
    RuntimeAssets::LoadedTexBinView loadedTex;
    RuntimeAssets::LoadedAssetBinView loadedAsset;
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
