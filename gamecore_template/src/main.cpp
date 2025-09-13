#include <gamecore/gc_app.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_world.h>
#include <gamecore/gc_render_backend.h>
#include <gamecore/gc_cube_component.h>

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

    app.renderBackend().setSyncMode(gc::RenderSyncMode::VSYNC_ON_TRIPLE_BUFFERED);

    gc::Window& win = app.window();
    win.setTitle("Hello world!");
    win.setIsResizable(true);

    gc::World& world = app.world();
    std::array<gc::Entity, 100> cubes{};
    const gc::Entity parent = world.createEntity(gc::strToName("parent"), gc::ENTITY_NONE, glm::vec3{0.0f, 0.0f, 50.0f});
    for (int x = 0; x < 10; ++x) {
        for (int y = 0; y < 10; ++y) {
            auto& cube = cubes[x * 10 + y];
            cube = world.createEntity(gc::strToNameRuntime(std::format("cube{}.{}", x, y)), parent, glm::vec3{x * 3.0f - 15.0f, y * 3.0f - 15.0f, 0.0f});
            world.addComponent<gc::CubeComponent>(cube);
        }
    }

    win.setWindowVisibility(true);

    app.run();

    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame() therefore main() can always return 0
    return 0;
}
