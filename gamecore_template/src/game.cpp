#include "game.h"

#include "gamecore/gc_ecs.h"
#include "gen_mesh.h"
#include "spin.h"
#include "mouse_move.h"

#include <SDL3/SDL_stdinc.h>
#include <gamecore/gc_app.h>
#include <gamecore/gc_render_backend.h>
#include <gamecore/gc_world.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_content.h>
#include <gamecore/gc_cube_component.h>
#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_transform_component.h>

void buildAndStartGame(gc::App& app)
{
    SDL_srand(static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()));

    gc::Window& win = app.window();
    gc::World& world = app.world();
    gc::RenderBackend& render_backend = app.renderBackend();
    gc::Content& content = app.content();

    // On Windows/NVIDIA, TRIPLE_BUFFERED gives horrible latency and TRIPLE_BUFFERED_UNTHROTTLED doesn't work properly so use double buffering instead
#ifdef WIN32
    render_backend.setSyncMode(gc::RenderSyncMode::VSYNC_OFF);
#else
    render_backend.setSyncMode(gc::RenderSyncMode::VSYNC_ON_TRIPLE_BUFFERED_UNTHROTTLED);
#endif

    world.registerComponent<SpinComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<MouseMoveComponent, gc::ComponentArrayType::SPARSE>();
    world.registerSystem<SpinSystem>();
    world.registerSystem<MouseMoveSystem>();

    std::shared_ptr<gc::GPUPipeline> pipeline{};
    {
        auto vert = content.findAsset(gc::Name("fancy.vert"));
        auto frag = content.findAsset(gc::Name("fancy.frag"));
        if (!vert.empty() && !frag.empty()) {
            pipeline = std::make_shared<gc::GPUPipeline>(render_backend.createPipeline(vert, frag));
        }
        else {
            gc::abortGame("Couldn't find fancy.vert or fancy.frag");
        }
    }

    std::array texture_names{gc::Name("box.jpg"),  gc::Name("bricks.jpg"), gc::Name("fire.jpg"),
                             gc::Name("nuke.jpg"), gc::Name("moss.png"),   gc::Name("uvcheck.png")};
    std::array<std::unique_ptr<gc::RenderMaterial>, texture_names.size() + 1> materials{};
    {
        std::shared_ptr<gc::RenderTexture> occlusion_roughness_metallic_texture{};
        {
            std::array<uint8_t, 4 + 4 + 4> image_data{}; // uint32 width, uint32 height, and one RGBA pixel
            image_data[0] = 1u;                          // width = 1
            image_data[4] = 1u;                          // height = 1
            image_data[8] = 255u;                        // R channel occlusion
            image_data[9] = 200u;                        // G channel roughness
            image_data[10] = 0u;                         // B channel metallic
            image_data[11] = 0u;                         // unused alpha channel
            occlusion_roughness_metallic_texture = std::make_shared<gc::RenderTexture>(render_backend.createTexture(image_data, false));
        }
        std::shared_ptr<gc::RenderTexture> normal_texture{};
        {
            std::array<uint8_t, 4 + 4 + 4> image_data{}; // uint32 width, uint32 height, and one RGBA pixel
            image_data[0] = 1u;                          // width = 1
            image_data[4] = 1u;                          // height = 1
            image_data[8] = 128u;                        // R channel (tangent space X)
            image_data[9] = 128u;                        // G channel (tangent space Y)
            image_data[10] = 255u;                       // B channel (tangent space Z)
            image_data[11] = 0u;                         // unused alpha channel
            normal_texture = std::make_shared<gc::RenderTexture>(render_backend.createTexture(image_data, false));
        }
        for (int i = 0; i < texture_names.size(); ++i) {
            const gc::Name texture_name = texture_names[i];
            auto image_data = content.findAsset(texture_name);
            if (!image_data.empty()) {
                materials[i] = std::make_unique<gc::RenderMaterial>(
                    render_backend.createMaterial(std::make_shared<gc::RenderTexture>(render_backend.createTexture(image_data, true)),
                                                  occlusion_roughness_metallic_texture, normal_texture, pipeline));
            }
            else {
                gc::abortGame("Couldn't find {}", texture_name.getString());
            }
        }
    }
    {
        auto base_color = std::make_shared<gc::RenderTexture>(render_backend.createTexture(content.findAsset(gc::Name("bricks-mortar-albedo.png")), true));
        auto orm = std::make_shared<gc::RenderTexture>(render_backend.createTexture(content.findAsset(gc::Name("bricks-mortar-orm.png")), false));
        auto normal = std::make_shared<gc::RenderTexture>(render_backend.createTexture(content.findAsset(gc::Name("bricks-mortar-normal.png")), false));
        materials.back() = std::make_unique<gc::RenderMaterial>(render_backend.createMaterial(base_color, orm, normal, pipeline));
    }

    auto cube_mesh = genCuboidMesh(app.renderBackend(), 1.0f, 1.0f, 1.0f);
    auto sphere_mesh = genSphereMesh(app.renderBackend(), 1.0f, 24);

    std::array<gc::Entity, 36> cubes{};
    const gc::Entity parent = world.createEntity(gc::Name("parent"), gc::ENTITY_NONE, glm::vec3{0.0f, 15.0f, 0.0f});
    world.addComponent<SpinComponent>(parent).setAxis({0.3f, 0.4f, 1.0f}).setRadiansPerSecond(0.05f);

    for (int x = 0; x < 6; ++x) {
        for (int y = 0; y < 6; ++y) {
            auto& cube = cubes[x * 6 + y];
            cube = world.createEntity(gc::Name(std::format("cube{}.{}", x, y)), parent, glm::vec3{(x - 2.5f) * 2.0f, 0.0f, (y - 2.5f) * 2.0f});
            world.addComponent<gc::CubeComponent>(cube).setMesh(&sphere_mesh).setMaterial(materials[SDL_rand(static_cast<int32_t>(materials.size()))].get());
            world.addComponent<SpinComponent>(cube).setAxis({1.0f, 0.0f, 0.7f}).setRadiansPerSecond(-0.5f);
        }
    }

    // add a floor
    auto floor_mesh = genCuboidMesh(render_backend, 100.0f, 100.0f, 1.0f, 25.0f);
    const auto floor = world.createEntity(gc::Name("floor"), gc::ENTITY_NONE, {0.0f, 50.0f, -10.0f});
    world.addComponent<gc::CubeComponent>(floor).setMesh(&floor_mesh).setMaterial(materials.back().get());

    const auto cube = world.createEntity(gc::Name("cube"), gc::ENTITY_NONE, glm::vec3{0.0f, +5.0f, 0.0f});
    world.addComponent<gc::CubeComponent>(cube).setMaterial(materials[5].get()).setMesh(&cube_mesh);
    world.addComponent<SpinComponent>(cube).setAxis({1.0f, 0.0f, 0.0f}).setRadiansPerSecond(0.25f);

    auto light_pivot = world.createEntity(gc::Name("light_pivot"), gc::ENTITY_NONE, {0.0f, 20.0f, 5.0f});
    world.addComponent<SpinComponent>(light_pivot).setAxis({0.0f, 0.0f, 1.0f}).setRadiansPerSecond((2.0f));
    auto light_mesh = genSphereMesh(render_backend, 0.5f, 16, true);
    const auto light = world.createEntity(gc::Name("light"), light_pivot, {0.0f, 10.0f, 0.0f});
    world.addComponent<gc::CubeComponent>(light).setMesh(&light_mesh).setMaterial(materials[2].get());

    // camera
    auto camera = world.createEntity(gc::Name("camera"), gc::ENTITY_NONE, {0.0f, 0.0f, 0.0f});
    world.addComponent<gc::CameraComponent>(camera).setFOV(glm::radians(45.0f)).setActive(true);
    world.addComponent<MouseMoveComponent>(camera).setMoveSpeed(25.0f).setAcceleration(50.0f).setSensitivity(1e-3f);

    win.setTitle("Hello world!");
    win.setIsResizable(true);
    win.setMouseCaptured(true);
    win.setSize(0, 0, true);
    win.setWindowVisibility(true);

    app.run();
}
