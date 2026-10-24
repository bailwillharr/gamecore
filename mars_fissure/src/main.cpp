//
// mars_fissure.exe
//
// Walk, or fly, over a piece of Mars at full size: a fissure east of Olympus Mons, from a HiRISE digital terrain model, 6 x 12 km
// at a metre per sample. Shows the engine's terrains: streamed levels of detail, and collisions with them. See README.
//

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
        gclog::Logger::instance().setLogFile(options.log_file);
    }

    gc::AppInitOptions init_options{};
    init_options.name = "mars_fissure";
    init_options.author = "bailwillharr";
    init_options.version = "v0.1.0";
    // Only the package files that the game uses, rather than everything in the content directory
    init_options.pak_files_override = {"shaders.gcpak", "mars.gcpak", "mars_terrain.gcpak", "ui.gcpak"};

    gc::App::initialise(init_options);

    const int exit_code = buildAndStartGame(gc::App::instance(), options);

    gc::App::shutdown();

    return exit_code;
}
