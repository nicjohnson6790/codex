#include "PineTreePackConverter.hpp"

#include "AssetBinWriter.hpp"
#include "FbxImport.hpp"
#include "MeshBinWriter.hpp"
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
    const RuntimeAssets::LoadedMeshBinView& meshBin,
    const RuntimeAssets::LoadedTexBinView& texBin,
    const RuntimeAssets::LoadedAssetBinView& assetBin,
    std::string* error)
{
    for (const RuntimeAssets::MeshRefRecord& meshRef : assetBin.meshRefs)
    {
        if (meshRef.meshIndex >= meshBin.meshes.size())
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

    for (const RuntimeAssets::SubmeshRecord& submesh : meshBin.submeshes)
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

    return true;
}

} // namespace

bool PineTreePackConverter::run(const ConverterConfig& config, ConversionSummary* outSummary, std::string* error)
{
    ImportedPack pack;
    std::size_t fbxFileCount = 0;
    if (!ImportTextureFolder(config.textureRoot, &pack, error))
    {
        return false;
    }
    if (!ImportFbxFolder(config.fbxRoot, &pack, &fbxFileCount, error))
    {
        return false;
    }
    RebuildMaterialLayout(&pack);

    std::filesystem::create_directories(config.outputRoot);
    const std::filesystem::path meshBinPath = config.outputRoot / (config.packName + ".meshbin");
    const std::filesystem::path texBinPath = config.outputRoot / (config.packName + ".texbin");
    const std::filesystem::path assetBinPath = config.outputRoot / (config.packName + ".assetbin");

    std::uint64_t meshBinSize = 0;
    std::uint64_t texBinSize = 0;
    std::uint64_t assetBinSize = 0;
    if (!WriteMeshBin(pack, meshBinPath, &meshBinSize, error) ||
        !WriteTexBin(pack, texBinPath, &texBinSize, error) ||
        !WriteAssetBin(pack, config.packName, assetBinPath, &assetBinSize, error))
    {
        return false;
    }

    RuntimeAssets::LoadedMeshBinView loadedMesh;
    RuntimeAssets::LoadedTexBinView loadedTex;
    RuntimeAssets::LoadedAssetBinView loadedAsset;
    if (!RuntimeAssets::LoadMeshBinFromSDL(meshBinPath.string().c_str(), &loadedMesh, error) ||
        !RuntimeAssets::LoadTexBinFromSDL(texBinPath.string().c_str(), &loadedTex, error) ||
        !RuntimeAssets::LoadAssetBinFromSDL(assetBinPath.string().c_str(), &loadedAsset, error))
    {
        return false;
    }

    if (!ValidateCrossReferences(loadedMesh, loadedTex, loadedAsset, error))
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
