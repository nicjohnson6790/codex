#include "PineTreePackConverter.hpp"
#include "EtopoHeightmapConverter.hpp"

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

void ApplyPackDefaults(std::string_view packName, ConverterConfig* outConfig)
{
    outConfig->packName = std::string(packName);
    outConfig->outputRoot = repoRoot() / "assets" / "runtime";

    if (packName == "skybox")
    {
        outConfig->packKind = ConverterConfig::PackKind::Skybox;
        outConfig->sourceRoot = repoRoot() / "assets" / "source" / "skybox";
        outConfig->fbxRoot.clear();
        outConfig->textureRoot = outConfig->sourceRoot / "tex";
        return;
    }

    if (packName == "pbr")
    {
        outConfig->packKind = ConverterConfig::PackKind::Pbr;
        outConfig->sourceRoot = repoRoot() / "assets" / "source" / "pbr";
        outConfig->fbxRoot.clear();
        outConfig->textureRoot = outConfig->sourceRoot / "tex";
        return;
    }

    if (packName == "roboto")
    {
        outConfig->packKind = ConverterConfig::PackKind::Font;
        outConfig->sourceRoot = repoRoot() / "assets" / "source" / "font";
        outConfig->fbxRoot.clear();
        outConfig->textureRoot.clear();
        return;
    }

    outConfig->packKind = ConverterConfig::PackKind::PineTree;
    outConfig->sourceRoot = repoRoot() / "assets" / "source" / outConfig->packName;
    outConfig->fbxRoot = outConfig->sourceRoot / "fbx";
    outConfig->textureRoot = outConfig->sourceRoot / "tex";
}

void PrintUsage()
{
    std::cout
        << "Usage:\n"
        << "  converter.exe pinetreepack\n"
        << "  converter.exe skybox\n"
        << "  converter.exe pbr\n"
        << "  converter.exe roboto\n"
        << "  converter.exe etopo2022 [--source <tif>] [--out <directory>] [--verbose|--self-test]\n"
        << "  converter.exe --source <path> --out <path> --name <pack>\n";
}

bool RunEtopo(int argc, char** argv, std::string* error)
{
    EtopoConversionConfig config{
        repoRoot() / "assets" / "source" / "etopo2022" / "ETOPO_2022_v1_60s_N90W180_surface.tif",
        repoRoot() / "assets" / "runtime",
        false,
        false
    };
    for (int index = 2; index < argc; ++index)
    {
        const std::string_view arg = argv[index];
        if ((arg == "--source" || arg == "--out") && index + 1 >= argc)
        { *error = std::string(arg) + " requires a path"; return false; }
        if (arg == "--source") config.sourceTiff = argv[++index];
        else if (arg == "--out") config.outputRoot = argv[++index];
        else if (arg == "--verbose") config.verbose = true;
        else if (arg == "--self-test") config.selfTestOnly = true;
        else { *error = "unknown etopo2022 argument: " + std::string(arg); return false; }
    }
    EtopoHeightmapConverter converter;
    EtopoConversionSummary summary;
    return converter.run(config, &summary, error);
}

bool ParseArguments(int argc, char** argv, ConverterConfig* outConfig, std::string* error)
{
    ApplyPackDefaults("pinetreepack", outConfig);

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
            ApplyPackDefaults(argv[++index], outConfig);
        }
        else if (arg == "--help" || arg == "-h")
        {
            PrintUsage();
            return false;
        }
        else if (!arg.empty() && arg[0] != '-')
        {
            ApplyPackDefaults(arg, outConfig);
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
    std::cout << std::unitbuf;

    if (!SDL_Init(0))
    {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
        return 1;
    }

    if (argc >= 2 && std::string_view(argv[1]) == "etopo2022")
    {
        std::string etopoError;
        const bool success = RunEtopo(argc, argv, &etopoError);
        if (!success) std::cerr << "ETOPO conversion failed: " << etopoError << '\n';
        SDL_Quit();
        return success ? 0 : 1;
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
    std::cout << "Converting pack: " << config.packName << '\n';
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
