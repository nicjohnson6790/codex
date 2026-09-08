#include "App.hpp"

#include <charconv>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>

#if defined(_WIN32)
extern "C"
{
    __declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

int main(int argc, char **argv)
{
    try
    {
        {
            std::ofstream logFile("launch.log", std::ios::trunc);
        }

        App::Options options{};
        for (int index = 1; index < argc; ++index)
        {
            if (std::strcmp(argv[index], "--verbose-startup") == 0)
            {
                options.verboseStartupLogging = true;
            }
            else if (std::strcmp(argv[index], "--quit-after-first-frame") == 0)
            {
                options.quitAfterFirstFrame = true;
            }
            else if (std::strcmp(argv[index], "--quit-after-frames") == 0 && index + 1 < argc)
            {
                const char *value = argv[++index];
                const char *end = value + std::strlen(value);
                if (const auto result = std::from_chars(value, end, options.quitAfterFrameCount);
                    result.ec != std::errc{} || result.ptr != end || options.quitAfterFrameCount == 0)
                {
                    std::cerr << "Invalid --quit-after-frames value\n";
                    return 2;
                }
            }
            else if (std::strcmp(argv[index], "--disable-steam") == 0)
            {
                options.enableSteam = false;
            }
            else if (std::strcmp(argv[index], "--verify-clouds") == 0)
            {
                options.verifyClouds = true;
            }
            else if (std::strcmp(argv[index], "--verify-heightmap-pipeline") == 0)
            {
                options.verifyHeightmapPipeline = true;
            }
            else if (std::strcmp(argv[index], "--stress-heightmap-pipeline") == 0)
            {
                options.verifyHeightmapPipeline = true;
                options.stressHeightmapPipeline = true;
            }
            else
            {
                std::cerr << "Unknown argument: " << argv[index] << '\n';
                return 2;
            }
        }

        std::unique_ptr<App> app = std::make_unique<App>(options);
        app->run();
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::ofstream logFile("launch.log", std::ios::app);
        if (logFile)
        {
            logFile << "[exception] " << exception.what() << '\n';
        }
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
