#include "game.h"

#include "gen_mesh.h"
#include "spin.h"
#include "mouse_move.h"

#include <gamecore/gc_app.h>
#include <gamecore/gc_render_backend.h>
#include <gamecore/gc_world.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_content.h>
#include <gamecore/gc_cube_component.h>

void buildAndStartGame(gc::App& app)
{
    gc::Window& win = app.window();
    gc::World& world = app.world();
    gc::RenderBackend& render_backend = app.renderBackend();
    gc::Content& content = app.content();

    // On Windows/NVIDIA, triple buffered gives horrible latency so use double buffering instead
    render_backend.setSyncMode(gc::RenderSyncMode::VSYNC_ON_TRIPLE_BUFFERED_UNTHROTTLED);

    world.registerComponent<SpinComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<MouseMoveComponent, gc::ComponentArrayType::SPARSE>();
    world.registerSystem<SpinSystem>();
    world.registerSystem<MouseMoveSystem>();

    std::array<std::unique_ptr<gc::RenderMaterial>, 5> materials{};
    {
        std::shared_ptr<gc::GPUPipeline> pipeline{};
        {
            auto vert = content.findAsset(gc::Name("fancy.vert"));
            auto frag = content.findAsset(gc::Name("fancy.frag"));
            if (!vert.empty() && !frag.empty()) {
                pipeline = std::make_shared<gc::GPUPipeline>(render_backend.createPipeline(vert, frag));
            }
            else {
                gc::abortGame("Couldn't find cube.vert or cube.frag");
            }
        }
        std::array<gc::Name, 5> texture_names{gc::Name("box.jpg"), gc::Name("bricks.jpg"), gc::Name("fire.jpg"), gc::Name("moss.png"), gc::Name("nuke.jpg")};
        for (int i = 0; i < 5; ++i) {
            const gc::Name texture_name = texture_names[i];
            auto image_data = content.findAsset(texture_name);
            if (!image_data.empty()) {
                materials[i] = std::make_unique<gc::RenderMaterial>(
                    render_backend.createMaterial(std::make_shared<gc::RenderTexture>(render_backend.createTexture(image_data)), pipeline));
            }
            else {
                gc::abortGame("Couldn't find {}", texture_name.getString());
            }
        }
    }

    auto cube_mesh = genCuboidMesh(app.renderBackend(), 1.0f, 1.0f, 1.0f);
    auto sphere_mesh = genSphereMesh(app.renderBackend(), 1.0f, 24);

    std::array<gc::Entity, 36> cubes{};
    const gc::Entity parent = world.createEntity(gc::Name("parent"), gc::ENTITY_NONE, glm::vec3{0.0f, 0.0f, 15.0f});
    world.addComponent<SpinComponent>(parent);
    for (int x = 0; x < 6; ++x) {
        for (int y = 0; y < 6; ++y) {
            auto& cube = cubes[x * 6 + y];
            cube = world.createEntity(gc::Name(std::format("cube{}.{}", x, y)), parent, glm::vec3{x * 3.0f - 9.0f, y * 3.0f - 9.0f, 0.0f});
            world.addComponent<gc::CubeComponent>(cube).setMesh(&cube_mesh).setMaterial(materials[rand() % 5].get());
            world.addComponent<SpinComponent>(cube).setAxis({1.0f, 0.0f, 0.0f}).setRadiansPerSecond(-2.0f);
        }
    }
    world.deleteEntity(cubes[10]);
    const auto another_entity = world.createEntity(gc::Name("another entity"), gc::ENTITY_NONE, {0.0f, 0.0f, 10.0f});
    world.addComponent<SpinComponent>(another_entity);
    world.addComponent<MouseMoveComponent>(another_entity).sensitivity = 0.01f;
    world.addComponent<gc::CubeComponent>(another_entity).setMesh(&sphere_mesh).setMaterial(materials[2].get());

    win.setTitle("Hello world!");
    win.setIsResizable(true);
    win.setMouseCaptured(true);
    win.setSize(0, 0, true);
    win.setWindowVisibility(true);

    app.run();
}
