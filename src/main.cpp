#include "App.hpp"

#include <fstream>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>

int main(int argc, char** argv)
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
    catch (const std::exception& exception)
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
