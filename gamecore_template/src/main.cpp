#include <gamecore/gc_app.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_world.h>
#include <gamecore/gc_core_components.h>

#include <SDL3/SDL_main.h>

struct CmdLineOptions {};

static CmdLineOptions parseCmdLine(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    return {};
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    const CmdLineOptions cmd_line_options = parseCmdLine(argc, argv);

    gc::AppInitOptions init_options{};
    init_options.name = "gamecore_template";
    init_options.author = "bailwillharr";
    init_options.version = "v0.0.0";

    gc::App::initialise(init_options);

    gc::App& app = gc::app();

    gc::Window& win = app.window();
    win.setTitle("Hello world!");
    win.setIsResizable(true);
    win.setWindowVisibility(true);

    app.run();

    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame() therefore main() can always return 0
    return 0;
}
