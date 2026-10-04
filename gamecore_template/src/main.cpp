#include <SDL3/SDL_main.h>

#include <span>

#include <gclog/gclog.h>

#include <gamecore/gc_app.h>

#include "game.h"

// See game.h for the command line options
int main(int argc, char* argv[])
{
    const Options options = parseCommandLine(std::span<const char* const>(argv + 1, static_cast<size_t>(argc - 1)));

    if (!options.log_file.empty()) {
        // The first log file to be set is the one that gets used
        gclog::Logger::instance().setLogFile(options.log_file);
    }

    gc::AppInitOptions init_options{};
    init_options.name = "gamecore_template";
    init_options.author = "bailwillharr";
    init_options.version = "v0.0.0";
    init_options.headless = isHeadless(options);

    gc::App::initialise(init_options);

    const int exit_code = buildAndStartGame(gc::App::instance(), options);

    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame(), so this is only non-zero if a test failed
    return exit_code;
}
