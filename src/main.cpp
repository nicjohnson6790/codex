#include "App.hpp"

#include <cstring>
#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
    try
    {
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

        App app(options);
        app.run();
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
