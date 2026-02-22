#include <SDL3/SDL_main.h>

#include "gamecore/gc_abort.h"
#include "gcpak_editor.h"

#include <filesystem>
#include <iostream>
#include <vector>
#include <format>
#include <system_error>

#include <glm/trigonometric.hpp>

#include <stb_image.h>

#include <gcpak/gcpak.h>
#include <gcpak/gcpak_prefab.h>

#include <gamecore/gc_logger.h>
#include <gamecore/gc_app.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_render_backend.h>
#include <gamecore/gc_world.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_light_component.h>
#include <gamecore/gc_render_system.h>
#include <gamecore/gc_camera_system.h>
#include <gamecore/gc_light_system.h>
#include <gamecore/gc_resource_manager.h>
#include <gamecore/gc_gen_mesh.h>

#include "editor_system.h"

static void initEditorWorld(gc::App& app, const std::filesystem::path& open_file)
{
    using namespace gc;
    using namespace gc::literals;

    auto& world = app.world();
    auto& resource_manager = app.resourceManager();
    auto& render_backend = app.renderBackend();
    auto& content_manager = app.content();
    auto& window = app.window();

    {
        // set up pipeline
        auto vertex_spv = content_manager.findAsset("editor.vert"_name, gcpak::GcpakAssetType::SPIRV_SHADER);
        auto fragment_spv = content_manager.findAsset("editor.frag"_name, gcpak::GcpakAssetType::SPIRV_SHADER);
        if (vertex_spv.empty() || fragment_spv.empty()) {
            abortGame("Failed to load vertex or fragment shader");
        }
        render_backend.createPipeline(vertex_spv, fragment_spv);
    }

    world.registerComponent<gc::RenderableComponent, gc::ComponentArrayType::DENSE>();
    world.registerComponent<gc::CameraComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::LightComponent, gc::ComponentArrayType::SPARSE>();

    world.registerSystem<gc::RenderSystem>(resource_manager, render_backend);
    world.registerSystem<gc::CameraSystem>();
    world.registerSystem<gc::LightSystem>();

    world.registerSystem<EditorSystem>(window, resource_manager, open_file);

    {
        auto camera = world.createEntity("camera"_name);
        world.getComponent<TransformComponent>(camera)->setRotation(glm::angleAxis(glm::radians(90.0f), glm::vec3{1.0f, 0.0f, 0.0f}));
        world.addComponent<CameraComponent>(camera);
        world.addComponent<LightComponent>(camera);
    }
}

int main(int argc, char* argv[])
{
    gc::AppInitOptions options{};
    options.name = "gcpak_editor";
    options.version = "v0.1.0";
    options.author = "bailwillharr";

    // Only load the shaders file, to allow the other pak files to be written.
    options.pak_files_override.push_back("shaders.gcpak");

    gc::App::initialise(options);

    auto& app = gc::App::instance();

    std::filesystem::path open_file{};
    if (argc >= 2) {
        open_file = argv[1];
    }
    initEditorWorld(app, open_file);

    auto& render_backend = app.renderBackend();
    render_backend.setSyncMode(gc::RenderSyncMode::VSYNC_ON_DOUBLE_BUFFERED);

    auto& window = app.window();
    window.setTitle("Gcpak Editor");
    window.setIsResizable(true);
    window.setWindowVisibility(true);

    app.run();

    gc::App::shutdown();

    return 0;
}
