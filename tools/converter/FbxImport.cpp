#include "FbxImport.hpp"

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/matrix3x3.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
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

std::optional<std::uint32_t> InferExplicitLodIndex(std::string_view name)
{
    if (ToLower(std::string(name)).find("billboard") != std::string::npos)
    {
        return 3u;
    }

    std::regex lodRegex("lod\\s*([0-9]+)", std::regex::icase);
    std::smatch match;
    const std::string ownedName(name);
    if (std::regex_search(ownedName, match, lodRegex))
    {
        return static_cast<std::uint32_t>(std::stoul(match[1].str()));
    }

    return std::nullopt;
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
    case 3:
        return 200.0f;
    default:
        return 100.0f;
    }
}

bool EqualsInsensitive(std::string_view lhs, std::string_view rhs)
{
    if (lhs.size() != rhs.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < lhs.size(); ++i)
    {
        if (static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(lhs[i]))) !=
            static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(rhs[i]))))
        {
            return false;
        }
    }

    return true;
}

bool ContainsInsensitive(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
    {
        return true;
    }
    const std::string loweredHaystack = ToLower(std::string(haystack));
    const std::string loweredNeedle = ToLower(std::string(needle));
    return loweredHaystack.find(loweredNeedle) != std::string::npos;
}

std::string TrimMaterialSuffix(std::string value)
{
    static constexpr std::string_view kSuffixes[] = {
        "_mat",
        "_material",
    };

    for (std::string_view suffix : kSuffixes)
    {
        if (value.size() >= suffix.size() &&
            value.compare(value.size() - suffix.size(), suffix.size(), suffix.data()) == 0)
        {
            value.resize(value.size() - suffix.size());
            break;
        }
    }
    return value;
}

std::uint32_t FindTextureByTokens(
    const ImportedPack& pack,
    std::initializer_list<std::string_view> requiredTokens,
    std::initializer_list<std::string_view> rejectedTokens = {})
{
    for (std::uint32_t textureIndex = 0; textureIndex < pack.textures.size(); ++textureIndex)
    {
        const ImportedTexture& texture = pack.textures[textureIndex];
        bool matches = true;
        for (std::string_view token : requiredTokens)
        {
            if (!ContainsInsensitive(texture.name, token))
            {
                matches = false;
                break;
            }
        }
        if (!matches)
        {
            continue;
        }
        for (std::string_view token : rejectedTokens)
        {
            if (ContainsInsensitive(texture.name, token))
            {
                matches = false;
                break;
            }
        }
        if (matches)
        {
            return textureIndex;
        }
    }
    return std::numeric_limits<std::uint32_t>::max();
}

void ResetMaterialTextureAssignments(ImportedMaterial* material)
{
    material->record.baseColorTextureIndex = std::numeric_limits<std::uint32_t>::max();
    material->record.normalTextureIndex = std::numeric_limits<std::uint32_t>::max();
    material->record.roughnessTextureIndex = std::numeric_limits<std::uint32_t>::max();
    material->record.specularTextureIndex = std::numeric_limits<std::uint32_t>::max();
    material->record.aoTextureIndex = std::numeric_limits<std::uint32_t>::max();
    material->record.subsurfaceTextureIndex = std::numeric_limits<std::uint32_t>::max();
    material->record.displacementTextureIndex = std::numeric_limits<std::uint32_t>::max();
}

void AssignTextureByTokens(
    const ImportedPack& pack,
    std::uint32_t* field,
    std::initializer_list<std::string_view> requiredTokens,
    std::initializer_list<std::string_view> rejectedTokens = {})
{
    *field = FindTextureByTokens(pack, requiredTokens, rejectedTokens);
}

void ApplyPackSpecificMaterialInference(ImportedPack* pack, ImportedMaterial* imported)
{
    const std::string& lowerName = imported->normalizedName;
    const bool isBillboard = ContainsInsensitive(lowerName, "billboard");
    const bool isNeedles = ContainsInsensitive(lowerName, "pine_sylvestris") || ContainsInsensitive(lowerName, "needle");

    if (ContainsInsensitive(lowerName, "barktop"))
    {
        ResetMaterialTextureAssignments(imported);
        AssignTextureByTokens(*pack, &imported->record.baseColorTextureIndex, { "barktop", "color" });
        AssignTextureByTokens(*pack, &imported->record.normalTextureIndex, { "barktop", "normal" });
        AssignTextureByTokens(*pack, &imported->record.roughnessTextureIndex, { "barktop", "roughness" });
        AssignTextureByTokens(*pack, &imported->record.specularTextureIndex, { "barktop", "specular" });
        AssignTextureByTokens(*pack, &imported->record.aoTextureIndex, { "barktop", "ao" });
    }
    else if (ContainsInsensitive(lowerName, "bark_bottom"))
    {
        ResetMaterialTextureAssignments(imported);
        AssignTextureByTokens(*pack, &imported->record.baseColorTextureIndex, { "bark_bottom", "color" });
        AssignTextureByTokens(*pack, &imported->record.normalTextureIndex, { "bark_bottom", "normal" });
        AssignTextureByTokens(*pack, &imported->record.roughnessTextureIndex, { "bark_bottom", "roughness" });
        AssignTextureByTokens(*pack, &imported->record.specularTextureIndex, { "bark_bottom", "specular" });
        AssignTextureByTokens(*pack, &imported->record.aoTextureIndex, { "bark_bottom", "ao" });
        AssignTextureByTokens(*pack, &imported->record.displacementTextureIndex, { "bark_bottom", "displacement" });
    }
    else if (ContainsInsensitive(lowerName, "old_branch"))
    {
        ResetMaterialTextureAssignments(imported);
        AssignTextureByTokens(*pack, &imported->record.baseColorTextureIndex, { "old_branch", "color" }, { "roughness" });
        AssignTextureByTokens(*pack, &imported->record.normalTextureIndex, { "old_branch", "normal" });
        AssignTextureByTokens(*pack, &imported->record.roughnessTextureIndex, { "old_branch", "roughness" });
        AssignTextureByTokens(*pack, &imported->record.specularTextureIndex, { "old_branch", "specular" });
        AssignTextureByTokens(*pack, &imported->record.aoTextureIndex, { "old_branch", "ao" });
    }
    else if (isBillboard)
    {
        ResetMaterialTextureAssignments(imported);
        const std::string billboardPrefix = TrimMaterialSuffix(lowerName);
        AssignTextureByTokens(*pack, &imported->record.baseColorTextureIndex, { billboardPrefix, "color" });
        AssignTextureByTokens(*pack, &imported->record.normalTextureIndex, { billboardPrefix, "normal" });
        AssignTextureByTokens(*pack, &imported->record.subsurfaceTextureIndex, { billboardPrefix, "subsurface" });
        if (imported->record.subsurfaceTextureIndex == std::numeric_limits<std::uint32_t>::max())
        {
            AssignTextureByTokens(*pack, &imported->record.subsurfaceTextureIndex, { billboardPrefix, "_ss" });
        }
    }
    else if (isNeedles)
    {
        ResetMaterialTextureAssignments(imported);
        AssignTextureByTokens(*pack, &imported->record.baseColorTextureIndex, { "pine_needles_atlas", "_bc" });
        AssignTextureByTokens(*pack, &imported->record.normalTextureIndex, { "pine_needles_atlas", "normal" });
        AssignTextureByTokens(*pack, &imported->record.roughnessTextureIndex, { "pine_needles_atlas", "roughness" });
        AssignTextureByTokens(*pack, &imported->record.specularTextureIndex, { "pine_needles_atlas", "specular" });
        AssignTextureByTokens(*pack, &imported->record.aoTextureIndex, { "pine_needles_atlas", "ao" });
        AssignTextureByTokens(*pack, &imported->record.subsurfaceTextureIndex, { "pine_needles", "subsurface" });
    }

    imported->record.flags &= ~(RuntimeAssets::MaterialFlagAlphaMasked |
        RuntimeAssets::MaterialFlagDoubleSided |
        RuntimeAssets::MaterialFlagSrgbBaseColor);
    imported->record.alphaCutoff = 0.0f;

    if (isNeedles || isBillboard)
    {
        imported->record.flags |= RuntimeAssets::MaterialFlagAlphaMasked;
        imported->record.flags |= RuntimeAssets::MaterialFlagDoubleSided;
        imported->record.alphaCutoff = 0.5f;
    }
    else if (ContainsInsensitive(lowerName, "bark_bottom"))
    {
        imported->record.flags |= RuntimeAssets::MaterialFlagAlphaMasked;
        imported->record.alphaCutoff = 0.5f;
    }

    if (imported->record.baseColorTextureIndex != std::numeric_limits<std::uint32_t>::max())
    {
        imported->record.flags |= RuntimeAssets::MaterialFlagSrgbBaseColor;
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

    ApplyPackSpecificMaterialInference(pack, &imported);

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
        importer.SetPropertyBool(AI_CONFIG_FBX_CONVERT_TO_M, true);
        importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
        const aiScene* scene = importer.ReadFile(
            fbxPath.string(),
            aiProcess_Triangulate |
            aiProcess_JoinIdenticalVertices |
            aiProcess_CalcTangentSpace |
            aiProcess_GenSmoothNormals |
            aiProcess_ImproveCacheLocality |
            aiProcess_GlobalScale |
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

        std::function<void(const aiNode*, const aiMatrix4x4&, std::optional<std::uint32_t>)> visitNode;
        const std::function<bool(const aiNode*)> hasExplicitLodNodes = [&](const aiNode* node) -> bool {
            if (node == nullptr)
            {
                return false;
            }
            if (InferExplicitLodIndex(node->mName.C_Str()).has_value())
            {
                return true;
            }
            for (unsigned childIndex = 0; childIndex < node->mNumChildren; ++childIndex)
            {
                if (hasExplicitLodNodes(node->mChildren[childIndex]))
                {
                    return true;
                }
            }
            return false;
        };
        const bool sceneHasExplicitLodNodes = hasExplicitLodNodes(scene->mRootNode);

        visitNode = [&](const aiNode* node, const aiMatrix4x4& parentTransform, std::optional<std::uint32_t> inheritedLod) {
            if (node == nullptr)
            {
                return;
            }

            const aiMatrix4x4 worldTransform = parentTransform * node->mTransformation;
            const std::optional<std::uint32_t> explicitLod = InferExplicitLodIndex(node->mName.C_Str());
            const std::optional<std::uint32_t> activeLod = explicitLod.has_value() ? explicitLod : inheritedLod;
            const bool emitMeshes = !sceneHasExplicitLodNodes || activeLod.has_value();

            if (emitMeshes)
            {
                aiMatrix3x3 normalTransform(worldTransform);
                normalTransform.Inverse().Transpose();

                for (unsigned nodeMeshIndex = 0; nodeMeshIndex < node->mNumMeshes; ++nodeMeshIndex)
                {
                    const unsigned sceneMeshIndex = node->mMeshes[nodeMeshIndex];
                    if (sceneMeshIndex >= scene->mNumMeshes)
                    {
                        continue;
                    }

                    const aiMesh* aiMeshPtr = scene->mMeshes[sceneMeshIndex];
                    if (aiMeshPtr == nullptr || aiMeshPtr->mNumVertices == 0 || aiMeshPtr->mNumFaces == 0)
                    {
                        continue;
                    }

                    ImportedMesh mesh;
                    mesh.sourceFile = fbxPath.filename().string();
                    mesh.meshName = node->mName.length > 0 ? node->mName.C_Str() :
                        (aiMeshPtr->mName.length > 0 ? aiMeshPtr->mName.C_Str() : assetName);
                    if (node->mNumMeshes > 1)
                    {
                        mesh.meshName += "_";
                        mesh.meshName += std::to_string(nodeMeshIndex);
                    }
                    mesh.assetName = assetName;
                    mesh.lodIndex = activeLod.value_or(InferLodIndex(mesh.meshName + "_" + assetName));
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
                        const aiVector3D transformedPosition = worldTransform * aiMeshPtr->mVertices[vertexIndex];

                        aiVector3D transformedNormal = aiMeshPtr->HasNormals()
                            ? (normalTransform * aiMeshPtr->mNormals[vertexIndex])
                            : aiVector3D(0.0f, 1.0f, 0.0f);
                        transformedNormal.NormalizeSafe();

                        aiVector3D transformedTangent = aiMeshPtr->HasTangentsAndBitangents()
                            ? (normalTransform * aiMeshPtr->mTangents[vertexIndex])
                            : aiVector3D(1.0f, 0.0f, 0.0f);
                        transformedTangent.NormalizeSafe();
                        aiVector3D transformedBitangent = aiMeshPtr->HasTangentsAndBitangents()
                            ? (normalTransform * aiMeshPtr->mBitangents[vertexIndex])
                            : aiVector3D(0.0f, 0.0f, 1.0f);
                        transformedBitangent.NormalizeSafe();

                        float tangentHandedness = 1.0f;
                        if (aiMeshPtr->HasTangentsAndBitangents())
                        {
                            const aiVector3D recomputedBitangent = transformedNormal ^ transformedTangent;
                            tangentHandedness = (recomputedBitangent * transformedBitangent) < 0.0f ? -1.0f : 1.0f;
                        }

                        const aiVector3D uv = aiMeshPtr->HasTextureCoords(0) ? aiMeshPtr->mTextureCoords[0][vertexIndex] : aiVector3D(0.0f, 0.0f, 0.0f);

                        vertex.position = { transformedPosition.x, transformedPosition.y, transformedPosition.z };
                        vertex.normal = { transformedNormal.x, transformedNormal.y, transformedNormal.z };
                        vertex.tangent = { transformedTangent.x, transformedTangent.y, transformedTangent.z, tangentHandedness };
                        vertex.uv0 = { uv.x, uv.y };
                        mesh.vertices.push_back(vertex);
                        AccumulateBounds(&mesh.boundsMin, &mesh.boundsMax, transformedPosition);
                    }

                    mesh.indices.reserve(aiMeshPtr->mNumFaces * 3u);
                    for (unsigned faceIndex = 0; faceIndex < aiMeshPtr->mNumFaces; ++faceIndex)
                    {
                        const aiFace& face = aiMeshPtr->mFaces[faceIndex];
                        if (face.mNumIndices != 3)
                        {
                            *error = "non-triangle face survived triangulation in " + fbxPath.string();
                            return;
                        }
                        mesh.indices.push_back(face.mIndices[0]);
                        mesh.indices.push_back(face.mIndices[1]);
                        mesh.indices.push_back(face.mIndices[2]);
                    }

                    FinalizeBounds(&mesh);
                    asset.meshIndices.push_back(static_cast<std::uint32_t>(outPack->meshes.size()));
                    outPack->meshes.push_back(std::move(mesh));
                }
            }

            for (unsigned childIndex = 0; childIndex < node->mNumChildren; ++childIndex)
            {
                visitNode(node->mChildren[childIndex], worldTransform, activeLod);
            }
        };

        visitNode(scene->mRootNode, aiMatrix4x4(), std::nullopt);
        if (error != nullptr && !error->empty())
        {
            return false;
        }
        if (asset.meshIndices.empty())
        {
            *error = "No drawable meshes were imported from " + fbxPath.string();
            return false;
        }
        std::stable_sort(
            asset.meshIndices.begin(),
            asset.meshIndices.end(),
            [&](std::uint32_t lhs, std::uint32_t rhs) {
                const ImportedMesh& left = outPack->meshes[lhs];
                const ImportedMesh& right = outPack->meshes[rhs];
                if (left.lodIndex != right.lodIndex)
                {
                    return left.lodIndex < right.lodIndex;
                }
                return left.materialIndex < right.materialIndex;
            });

        asset.materialIndices.assign(materialSet.begin(), materialSet.end());
        std::sort(asset.materialIndices.begin(), asset.materialIndices.end());
        outPack->assets.push_back(std::move(asset));
    }

    return true;
}
