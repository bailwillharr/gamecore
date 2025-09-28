#include "game.h"

#include "gen_mesh.h"
#include "spin.h"
#include "mouse_move.h"

#include <gamecore/gc_app.h>
#include <gamecore/gc_render_backend.h>
#include <gamecore/gc_world.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_cube_component.h>

void buildAndStartGame(gc::App& app)
{
    gc::Window& win = app.window();
    gc::World& world = app.world();
    gc::RenderBackend& render_backend = app.renderBackend();

    // On Windows/NVIDIA, triple buffered gives horrible latency so use double buffering instead
    render_backend.setSyncMode(gc::RenderSyncMode::VSYNC_ON_DOUBLE_BUFFERED);

    world.registerComponent<SpinComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<MouseMoveComponent, gc::ComponentArrayType::SPARSE>();
    world.registerSystem<SpinSystem>();
    world.registerSystem<MouseMoveSystem>();

    auto mesh = genSphereMesh(app.renderBackend(), 1.0f, 12);

    std::array<gc::Entity, 36> cubes{};
    const gc::Entity parent = world.createEntity(gc::strToName("parent"), gc::ENTITY_NONE, glm::vec3{0.0f, 0.0f, 25.0f});
    world.addComponent<SpinComponent>(parent);
    world.addComponent<MouseMoveComponent>(parent).sensitivity = 0.01f;
    for (int x = 0; x < 6; ++x) {
        for (int y = 0; y < 6; ++y) {
            auto& cube = cubes[x * 6 + y];
            cube = world.createEntity(gc::strToNameRuntime(std::format("cube{}.{}", x, y)), parent, glm::vec3{x * 3.0f - 9.0f, y * 3.0f - 9.0f, 0.0f});
            world.addComponent<gc::CubeComponent>(cube).mesh = &mesh;
            world.addComponent<SpinComponent>(cube).setAxis({1.0f, 0.0f, 0.0f}).setRadiansPerSecond(-2.0f);
        }
    }
    world.deleteEntity(cubes[10]);
    const auto another_entity = world.createEntity(gc::strToName("another entity"), gc::ENTITY_NONE, {0.0f, 0.0f, 10.0f});
    world.addComponent<SpinComponent>(another_entity);
    world.addComponent<gc::CubeComponent>(another_entity).mesh = &mesh;

    win.setTitle("Hello world!");
    win.setIsResizable(true);
    win.setMouseCaptured(true);
    win.setSize(0, 0, true);
    win.setWindowVisibility(true);
    app.run();
}
