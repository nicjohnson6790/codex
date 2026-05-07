#include "PineTreePackConverter.hpp"

#include <SDL3/SDL.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace
{

std::filesystem::path repoRoot()
{
    return std::filesystem::path(CONVERTER_REPO_ROOT);
}

void PrintUsage()
{
    std::cout
        << "Usage:\n"
        << "  converter.exe pinetreepack\n"
        << "  converter.exe --source <path> --out <path> --name <pack>\n";
}

bool ParseArguments(int argc, char** argv, ConverterConfig* outConfig, std::string* error)
{
    outConfig->packName = "pinetreepack";
    outConfig->sourceRoot = repoRoot() / "assets" / "source" / "pinetreepack";
    outConfig->outputRoot = repoRoot() / "assets" / "runtime";
    outConfig->fbxRoot = outConfig->sourceRoot / "fbx";
    outConfig->textureRoot = outConfig->sourceRoot / "tex";

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view arg = argv[index];
        if (arg == "--source")
        {
            if (index + 1 >= argc)
            {
                *error = "--source requires a path";
                return false;
            }
            outConfig->sourceRoot = argv[++index];
            outConfig->fbxRoot = outConfig->sourceRoot / "fbx";
            outConfig->textureRoot = outConfig->sourceRoot / "tex";
        }
        else if (arg == "--out")
        {
            if (index + 1 >= argc)
            {
                *error = "--out requires a path";
                return false;
            }
            outConfig->outputRoot = argv[++index];
        }
        else if (arg == "--name")
        {
            if (index + 1 >= argc)
            {
                *error = "--name requires a value";
                return false;
            }
            outConfig->packName = argv[++index];
        }
        else if (arg == "--help" || arg == "-h")
        {
            PrintUsage();
            return false;
        }
        else if (!arg.empty() && arg[0] != '-')
        {
            outConfig->packName = std::string(arg);
            outConfig->sourceRoot = repoRoot() / "assets" / "source" / outConfig->packName;
            outConfig->outputRoot = repoRoot() / "assets" / "runtime";
            outConfig->fbxRoot = outConfig->sourceRoot / "fbx";
            outConfig->textureRoot = outConfig->sourceRoot / "tex";
        }
        else
        {
            *error = "unknown argument: " + std::string(arg);
            return false;
        }
    }

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (!SDL_Init(0))
    {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
        return 1;
    }

    ConverterConfig config;
    std::string error;
    if (!ParseArguments(argc, argv, &config, &error))
    {
        if (!error.empty())
        {
            std::cerr << error << '\n';
        }
        SDL_Quit();
        return error.empty() ? 0 : 2;
    }

    PineTreePackConverter converter;
    ConversionSummary summary;
    if (!converter.run(config, &summary, &error))
    {
        std::cerr << "Conversion failed: " << error << '\n';
        SDL_Quit();
        return 1;
    }

    std::cout << "Converted pack: " << config.packName << '\n';
    std::cout << "  FBX files: " << summary.fbxFileCount << '\n';
    std::cout << "  Meshes: " << summary.meshCount << '\n';
    std::cout << "  Vertices: " << summary.vertexCount << '\n';
    std::cout << "  Indices: " << summary.indexCount << '\n';
    std::cout << "  Textures: " << summary.textureCount << '\n';
    std::cout << "  Materials: " << summary.materialCount << '\n';
    std::cout << "  Mesh bin bytes: " << summary.meshBinSize << '\n';
    std::cout << "  Tex bin bytes: " << summary.texBinSize << '\n';
    std::cout << "  Asset bin bytes: " << summary.assetBinSize << '\n';
    std::cout << "  Validation: success\n";

    SDL_Quit();
    return 0;
}
