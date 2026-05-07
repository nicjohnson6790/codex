#include "FbxImport.hpp"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace
{

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string NormalizeName(std::string value)
{
    std::replace(value.begin(), value.end(), ' ', '_');
    return ToLower(value);
}

std::uint32_t InferLodIndex(const std::string& name)
{
    std::regex lodRegex("lod\\s*([0-9]+)", std::regex::icase);
    std::smatch match;
    if (std::regex_search(name, match, lodRegex))
    {
        return static_cast<std::uint32_t>(std::stoul(match[1].str()));
    }
    return 0;
}

float DefaultLodDistance(std::uint32_t lodIndex)
{
    switch (lodIndex)
    {
    case 0:
        return 25.0f;
    case 1:
        return 50.0f;
    case 2:
        return 100.0f;
    default:
        return 100.0f;
    }
}

std::uint32_t FindOrCreateMaterial(
    ImportedPack* pack,
    const aiMaterial* material,
    const std::string& fallbackName)
{
    std::string sourceName = fallbackName;
    if (material != nullptr)
    {
        aiString aiName;
        if (material->Get(AI_MATKEY_NAME, aiName) == aiReturn_SUCCESS && aiName.length > 0)
        {
            sourceName = aiName.C_Str();
        }
    }

    const std::string normalizedName = NormalizeName(sourceName);
    for (std::uint32_t index = 0; index < pack->materials.size(); ++index)
    {
        if (pack->materials[index].normalizedName == normalizedName)
        {
            return index;
        }
    }

    ImportedMaterial imported;
    imported.sourceName = sourceName;
    imported.normalizedName = normalizedName;
    imported.record.nameOffset = 0;
    imported.record.baseColorTextureIndex = std::numeric_limits<std::uint32_t>::max();
    imported.record.normalTextureIndex = std::numeric_limits<std::uint32_t>::max();
    imported.record.roughnessTextureIndex = std::numeric_limits<std::uint32_t>::max();
    imported.record.specularTextureIndex = std::numeric_limits<std::uint32_t>::max();
    imported.record.aoTextureIndex = std::numeric_limits<std::uint32_t>::max();
    imported.record.subsurfaceTextureIndex = std::numeric_limits<std::uint32_t>::max();
    imported.record.displacementTextureIndex = std::numeric_limits<std::uint32_t>::max();
    imported.record.flags = RuntimeAssets::MaterialFlagNone;
    imported.record.alphaCutoff = 0.0f;
    pack->materials.push_back(imported);
    return static_cast<std::uint32_t>(pack->materials.size() - 1);
}

void AccumulateBounds(
    std::array<float, 3>* outMin,
    std::array<float, 3>* outMax,
    const aiVector3D& position)
{
    (*outMin)[0] = std::min((*outMin)[0], position.x);
    (*outMin)[1] = std::min((*outMin)[1], position.y);
    (*outMin)[2] = std::min((*outMin)[2], position.z);
    (*outMax)[0] = std::max((*outMax)[0], position.x);
    (*outMax)[1] = std::max((*outMax)[1], position.y);
    (*outMax)[2] = std::max((*outMax)[2], position.z);
}

void FinalizeBounds(ImportedMesh* mesh)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        mesh->boundsCenter[axis] = (mesh->boundsMin[axis] + mesh->boundsMax[axis]) * 0.5f;
    }

    float maxDistanceSquared = 0.0f;
    for (const RuntimeAssets::MeshVertex& vertex : mesh->vertices)
    {
        const float dx = vertex.position[0] - mesh->boundsCenter[0];
        const float dy = vertex.position[1] - mesh->boundsCenter[1];
        const float dz = vertex.position[2] - mesh->boundsCenter[2];
        maxDistanceSquared = std::max(maxDistanceSquared, dx * dx + dy * dy + dz * dz);
    }
    mesh->boundsRadius = std::sqrt(maxDistanceSquared);
}

bool PopulateMaterialTextures(
    ImportedPack* pack,
    std::uint32_t materialIndex,
    const aiMaterial* material)
{
    if (material == nullptr)
    {
        return true;
    }

    ImportedMaterial& imported = pack->materials[materialIndex];
    auto assignTexture = [&](std::uint32_t* field, const std::string& path) {
        const std::string basename = ToLower(std::filesystem::path(path).filename().string());
        const auto it = pack->textureIndexByBasename.find(basename);
        if (it != pack->textureIndexByBasename.end())
        {
            *field = it->second;
        }
    };

    auto tryFirstTexture = [&](aiTextureType type, std::uint32_t* field) {
        if (*field != std::numeric_limits<std::uint32_t>::max())
        {
            return;
        }
        aiString path;
        if (material->GetTexture(type, 0, &path) == aiReturn_SUCCESS)
        {
            assignTexture(field, path.C_Str());
        }
    };

    tryFirstTexture(aiTextureType_BASE_COLOR, &imported.record.baseColorTextureIndex);
    tryFirstTexture(aiTextureType_DIFFUSE, &imported.record.baseColorTextureIndex);
    tryFirstTexture(aiTextureType_NORMALS, &imported.record.normalTextureIndex);
    tryFirstTexture(aiTextureType_NORMAL_CAMERA, &imported.record.normalTextureIndex);
    tryFirstTexture(aiTextureType_SPECULAR, &imported.record.specularTextureIndex);
    tryFirstTexture(aiTextureType_AMBIENT_OCCLUSION, &imported.record.aoTextureIndex);
    tryFirstTexture(aiTextureType_HEIGHT, &imported.record.displacementTextureIndex);
    tryFirstTexture(aiTextureType_DISPLACEMENT, &imported.record.displacementTextureIndex);
    tryFirstTexture(aiTextureType_OPACITY, &imported.record.subsurfaceTextureIndex);

    const std::string lowerName = imported.normalizedName;
    for (const ImportedTexture& texture : pack->textures)
    {
        if (texture.normalizedBasename.find(lowerName) == std::string::npos &&
            lowerName.find(texture.normalizedBasename) == std::string::npos)
        {
            continue;
        }

        const std::uint32_t textureIndex = pack->textureIndexByBasename.at(texture.normalizedBasename);
        if ((texture.name.find("_color") != std::string::npos || texture.name.find("_bc") != std::string::npos) &&
            imported.record.baseColorTextureIndex == std::numeric_limits<std::uint32_t>::max())
        {
            imported.record.baseColorTextureIndex = textureIndex;
        }
        else if (texture.name.find("_normal") != std::string::npos &&
            imported.record.normalTextureIndex == std::numeric_limits<std::uint32_t>::max())
        {
            imported.record.normalTextureIndex = textureIndex;
        }
        else if ((texture.name.find("_roughness") != std::string::npos ||
            texture.name.find("color roughness") != std::string::npos) &&
            imported.record.roughnessTextureIndex == std::numeric_limits<std::uint32_t>::max())
        {
            imported.record.roughnessTextureIndex = textureIndex;
        }
        else if (texture.name.find("_specular") != std::string::npos &&
            imported.record.specularTextureIndex == std::numeric_limits<std::uint32_t>::max())
        {
            imported.record.specularTextureIndex = textureIndex;
        }
        else if (texture.name.find("_ao") != std::string::npos &&
            imported.record.aoTextureIndex == std::numeric_limits<std::uint32_t>::max())
        {
            imported.record.aoTextureIndex = textureIndex;
        }
        else if ((texture.name.find("_subsurface") != std::string::npos || texture.name.find("_ss") != std::string::npos) &&
            imported.record.subsurfaceTextureIndex == std::numeric_limits<std::uint32_t>::max())
        {
            imported.record.subsurfaceTextureIndex = textureIndex;
        }
        else if (texture.name.find("_displacement") != std::string::npos &&
            imported.record.displacementTextureIndex == std::numeric_limits<std::uint32_t>::max())
        {
            imported.record.displacementTextureIndex = textureIndex;
        }
    }

    const bool alphaMasked = lowerName.find("needle") != std::string::npos || lowerName.find("billboard") != std::string::npos;
    if (alphaMasked)
    {
        imported.record.flags |= RuntimeAssets::MaterialFlagAlphaMasked;
        imported.record.alphaCutoff = 0.5f;
    }

    if (imported.record.baseColorTextureIndex != std::numeric_limits<std::uint32_t>::max())
    {
        imported.record.flags |= RuntimeAssets::MaterialFlagSrgbBaseColor;
    }

    return true;
}

} // namespace

bool ImportFbxFolder(
    const std::filesystem::path& fbxRoot,
    ImportedPack* outPack,
    std::size_t* outFbxFileCount,
    std::string* error)
{
    if (!std::filesystem::exists(fbxRoot))
    {
        *error = "FBX source folder does not exist: " + fbxRoot.string();
        return false;
    }

    std::vector<std::filesystem::path> fbxFiles;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(fbxRoot))
    {
        if (entry.is_regular_file() && ToLower(entry.path().extension().string()) == ".fbx")
        {
            fbxFiles.push_back(entry.path());
        }
    }
    std::sort(fbxFiles.begin(), fbxFiles.end());
    *outFbxFileCount = fbxFiles.size();

    for (const std::filesystem::path& fbxPath : fbxFiles)
    {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(
            fbxPath.string(),
            aiProcess_Triangulate |
            aiProcess_JoinIdenticalVertices |
            aiProcess_CalcTangentSpace |
            aiProcess_GenSmoothNormals |
            aiProcess_ImproveCacheLocality |
            aiProcess_ValidateDataStructure);
        if (scene == nullptr)
        {
            *error = "Assimp failed to load " + fbxPath.string() + ": " + importer.GetErrorString();
            return false;
        }

        const std::string assetName = fbxPath.stem().string();
        ImportedAsset asset;
        asset.name = assetName;
        std::unordered_set<std::uint32_t> materialSet;

        for (unsigned meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
        {
            const aiMesh* aiMeshPtr = scene->mMeshes[meshIndex];
            if (aiMeshPtr == nullptr || aiMeshPtr->mNumVertices == 0 || aiMeshPtr->mNumFaces == 0)
            {
                continue;
            }

            ImportedMesh mesh;
            mesh.sourceFile = fbxPath.filename().string();
            mesh.meshName = aiMeshPtr->mName.length > 0 ? aiMeshPtr->mName.C_Str() : assetName;
            mesh.assetName = assetName;
            mesh.lodIndex = InferLodIndex(mesh.meshName + "_" + assetName);
            mesh.lodMaxDistance = DefaultLodDistance(mesh.lodIndex);
            mesh.boundsMin = {
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max()
            };
            mesh.boundsMax = {
                std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest()
            };

            aiMaterial* aiMaterialPtr = scene->mMaterials != nullptr && aiMeshPtr->mMaterialIndex < scene->mNumMaterials
                ? scene->mMaterials[aiMeshPtr->mMaterialIndex]
                : nullptr;
            mesh.materialIndex = FindOrCreateMaterial(outPack, aiMaterialPtr, assetName + "_material");
            PopulateMaterialTextures(outPack, mesh.materialIndex, aiMaterialPtr);
            materialSet.insert(mesh.materialIndex);

            mesh.vertices.reserve(aiMeshPtr->mNumVertices);
            for (unsigned vertexIndex = 0; vertexIndex < aiMeshPtr->mNumVertices; ++vertexIndex)
            {
                RuntimeAssets::MeshVertex vertex{};
                const aiVector3D position = aiMeshPtr->mVertices[vertexIndex];
                const aiVector3D normal = aiMeshPtr->HasNormals() ? aiMeshPtr->mNormals[vertexIndex] : aiVector3D(0.0f, 1.0f, 0.0f);
                const aiVector3D tangent = aiMeshPtr->HasTangentsAndBitangents() ? aiMeshPtr->mTangents[vertexIndex] : aiVector3D(1.0f, 0.0f, 0.0f);
                const aiVector3D uv = aiMeshPtr->HasTextureCoords(0) ? aiMeshPtr->mTextureCoords[0][vertexIndex] : aiVector3D(0.0f, 0.0f, 0.0f);

                vertex.position = { position.x, position.y, position.z };
                vertex.normal = { normal.x, normal.y, normal.z };
                vertex.tangent = { tangent.x, tangent.y, tangent.z, 1.0f };
                vertex.uv0 = { uv.x, uv.y };
                mesh.vertices.push_back(vertex);
                AccumulateBounds(&mesh.boundsMin, &mesh.boundsMax, position);
            }

            mesh.indices.reserve(aiMeshPtr->mNumFaces * 3u);
            for (unsigned faceIndex = 0; faceIndex < aiMeshPtr->mNumFaces; ++faceIndex)
            {
                const aiFace& face = aiMeshPtr->mFaces[faceIndex];
                if (face.mNumIndices != 3)
                {
                    *error = "non-triangle face survived triangulation in " + fbxPath.string();
                    return false;
                }
                mesh.indices.push_back(face.mIndices[0]);
                mesh.indices.push_back(face.mIndices[1]);
                mesh.indices.push_back(face.mIndices[2]);
            }

            FinalizeBounds(&mesh);
            asset.meshIndices.push_back(static_cast<std::uint32_t>(outPack->meshes.size()));
            outPack->meshes.push_back(std::move(mesh));
        }

        asset.materialIndices.assign(materialSet.begin(), materialSet.end());
        std::sort(asset.materialIndices.begin(), asset.materialIndices.end());
        outPack->assets.push_back(std::move(asset));
    }

    return true;
}
