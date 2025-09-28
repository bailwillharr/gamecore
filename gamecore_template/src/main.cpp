#include <gamecore/gc_app.h>

#include "game.h"

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    gc::AppInitOptions init_options{};
    init_options.name = "gamecore_template";
    init_options.author = "bailwillharr";
    init_options.version = "v0.0.0";

    gc::App::initialise(init_options);

    buildAndStartGame(gc::App::instance());

    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame() therefore main() can always return 0
    return 0;
}
