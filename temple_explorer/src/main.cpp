//
// temple_explorer.exe
//
// Loads a whole world from one prefab (content/temple.gcpak) and lets the player fly around it. See README.
//

#include <SDL3/SDL_main.h>

#include <cstdlib>

#include <string>
#include <vector>

#include <glm/trigonometric.hpp>

#include <gclog/gclog.h>

#include <gamecore/gc_abort.h>
#include <gamecore/gc_app.h>
#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_camera_system.h>
#include <gamecore/gc_content.h>
#include <gamecore/gc_light_component.h>
#include <gamecore/gc_light_system.h>
#include <gamecore/gc_prefab.h>
#include <gamecore/gc_render_backend.h>
#include <gamecore/gc_render_system.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_resource_manager.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_world.h>

#include "fly_camera.h"

using namespace gc::literals;

static void createPipelines(gc::App& app)
{
    gc::Content& content = app.content();
    const auto frag = content.findAsset("pbr.frag"_name);
    const auto single_draw_vert = content.findAsset("pbr_single_draw.vert"_name);
    const auto instanced_vert = content.findAsset("pbr_instanced.vert"_name);
    for (const auto* const shader : {&frag, &single_draw_vert, &instanced_vert}) {
        if (shader->data.empty() || shader->type != gcpak::GcpakAssetType::SPIRV_SHADER) {
            gc::abortGame("Failed to find shaders. Is content/shaders.gcpak missing?");
        }
    }
    app.renderBackend().createMainPipeline(single_draw_vert.data, frag.data);
    app.renderBackend().createInstancingPipeline(instanced_vert.data, frag.data);
}

static void buildWorld(gc::App& app, gc::Name prefab_name)
{
    gc::World& world = app.world();

    // Prefabs find components by name, so everything the prefab uses has to be registered before it is loaded.
    world.registerComponent<gc::RenderableComponent, gc::ComponentArrayType::DENSE>();
    world.registerComponent<gc::CameraComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::LightComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<FlyCameraComponent, gc::ComponentArrayType::SPARSE>();

    // Systems are updated in the order they are registered
    world.registerSystem<FlyCameraSystem>();
    world.registerSystem<gc::RenderSystem>(app.resourceManager(), app.renderBackend());
    world.registerSystem<gc::CameraSystem>();
    world.registerSystem<gc::LightSystem>();

    // The whole world is one prefab. It only names its meshes, materials and textures: the RenderSystem loads them from the
    // content when they are first drawn.
    std::vector<gc::Entity> entities{};
    if (gc::loadPrefab(app.content(), prefab_name, world, gc::ENTITY_NONE, &entities) == gc::ENTITY_NONE) {
        gc::abortGame("Failed to load the prefab {}. Is content/temple.gcpak missing?", prefab_name);
    }
    GC_INFO("Loaded prefab {}: {} entities", prefab_name, entities.size());

    // The player. Made after the world so that its light is the one that gets used (see FlyCameraSystem).
    // FlyCameraSystem moves it to a sensible place on the first frame.
    const gc::Entity player = world.createEntity("player"_name);
    world.addComponent<gc::CameraComponent>(player).setFOV(glm::radians(70.0f)).setNearPlane(0.05f).setActive(true);
    world.addComponent<gc::LightComponent>(player);
    world.addComponent<FlyCameraComponent>(player);
}

// An optional argument names the prefab to load instead: temple_explorer [PREFAB]
int main(int argc, char* argv[])
{
    const std::string prefab_name = (argc >= 2) ? argv[1] : "temple.glb";

    gc::AppInitOptions init_options{};
    init_options.name = "temple_explorer";
    init_options.author = "bailwillharr";
    init_options.version = "v0.1.0";

    gc::App::initialise(init_options);
    gc::App& app = gc::App::instance();

    app.renderBackend().setSyncMode(gc::RenderSyncMode::VSYNC_ON_DOUBLE_BUFFERED);
    createPipelines(app);

    buildWorld(app, gc::Name(prefab_name));

    app.window().setTitle("Temple explorer");
    app.window().setIsResizable(true);
    app.window().setMouseCaptured(true);
    app.window().setWindowVisibility(true);

    app.run();

    gc::App::shutdown();

    return EXIT_SUCCESS;
}
