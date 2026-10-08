//
// temple_explorer.exe
//
// Loads a whole world from one prefab (content/temple.gcpak) and lets the player fly around it. See README.
//

#include <SDL3/SDL_main.h>

#include <cmath>
#include <cstdlib>

#include <string>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <gclog/gclog.h>

#include <gamecore/gc_abort.h>
#include <gamecore/gc_app.h>
#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_camera_system.h>
#include <gamecore/gc_collider_component.h>
#include <gamecore/gc_collision_bvh_component.h>
#include <gamecore/gc_collision_system.h>
#include <gamecore/gc_content.h>
#include <gamecore/gc_light_component.h>
#include <gamecore/gc_light_system.h>
#include <gamecore/gc_prefab.h>
#include <gamecore/gc_render_backend.h>
#include <gamecore/gc_render_system.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_resource_manager.h>
#include <gamecore/gc_shadow_map_component.h>
#include <gamecore/gc_skybox_component.h>
#include <gamecore/gc_transform_component.h>
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
    app.renderBackend().setWorldShaders(single_draw_vert.data, instanced_vert.data, frag.data);

    // the sky is drawn by shaders of its own
    const auto sky_vert = content.findAsset("sky.vert"_name);
    const auto sky_frag = content.findAsset("sky.frag"_name);
    if (sky_vert.type != gcpak::GcpakAssetType::SPIRV_SHADER || sky_frag.type != gcpak::GcpakAssetType::SPIRV_SHADER) {
        gc::abortGame("Failed to find the sky's shaders. Does content/shaders.gcpak need to be packaged again?");
    }
    app.renderBackend().setSkyShaders(sky_vert.data, sky_frag.data);
}

static void buildWorld(gc::App& app, gc::Name prefab_name, const std::string& sky_name)
{
    gc::World& world = app.world();

    // Prefabs find components by name, so everything the prefab uses has to be registered before it is loaded.
    world.registerComponent<gc::RenderableComponent, gc::ComponentArrayType::DENSE>();
    world.registerComponent<gc::CameraComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::LightComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::ShadowMapComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::SkyboxComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::ColliderComponent, gc::ComponentArrayType::DENSE>(); // nearly everything in a glTF scene is solid
    world.registerComponent<gc::CollisionBvhComponent, gc::ComponentArrayType::SPARSE>(); // the collision trees that were baked
    world.registerComponent<FlyCameraComponent, gc::ComponentArrayType::SPARSE>();

    // Systems are updated in the order they are registered.
    // The camera collides with the world, so the CollisionSystem has to be up to date before the camera moves.
    world.registerSystem<gc::CollisionSystem>(app.resourceManager());
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

    // Lights are in physical units (lux and candela), so the camera's exposure has to suit how bright this world is.
    // Find roughly how much light its surfaces get, in lux.
    float illuminance = 0.0f;
    float sun_illuminance = 0.0f;
    bool has_ambient_light = false;
    world.forEach<gc::LightComponent>([&](gc::Entity, const gc::LightComponent& light) {
        // a point light is judged by what it gives a surface 3 metres away
        illuminance = glm::max(illuminance, (light.m_type == gc::LightType::POINT) ? light.m_intensity / 9.0f : light.m_intensity);
        has_ambient_light = has_ambient_light || (light.m_type == gc::LightType::AMBIENT);
        if (light.m_type == gc::LightType::DIRECTIONAL) {
            sun_illuminance = light.m_intensity;
        }
    });
    if (illuminance <= 0.0f) {
        // The prefab has no lights, so add a midday sun.
        // Lights shine along their -Z axis, as cameras look. This one shines down from above +X +Y.
        illuminance = 100000.0f;
        sun_illuminance = illuminance;
        const gc::Entity sun = world.createEntity("sun"_name);
        world.getComponent<gc::TransformComponent>(sun)->setRotation(
            glm::quatLookAt(glm::normalize(glm::vec3{-1.0f, -1.0f, -1.0f}), glm::vec3{0.0f, 0.0f, 1.0f}));
        world.addComponent<gc::LightComponent>(sun).setType(gc::LightType::DIRECTIONAL).setIntensity(illuminance);
    }
    bool has_sky = false;
    world.forEach<gc::SkyboxComponent>([&](gc::Entity, const gc::SkyboxComponent&) { has_sky = true; });
    if (!has_sky) {
        // glTF files have no sky. Add one: it is seen behind the world, and lights what the other lights don't reach, so that
        // it isn't black. It turns so that its sun is in the same compass direction as the world's directional light.
        //
        // How bright should it be? The sky's asset says how much light its sun gives compared to the rest of it (the sun was
        // measured and taken out when the sky was packaged), so the sky is made to suit the world's sun. If either has no sun,
        // a clear sky's usual share is used.
        float sky_illuminance = 0.15f * illuminance;
        if (const gc::ResourceCubeMap* const sky = app.resourceManager().get<gc::ResourceCubeMap>(gc::Name(sky_name)); sky) {
            if (sun_illuminance > 0.0f && sky->info.sun_illuminance > 0.0f) {
                sky_illuminance = sun_illuminance * sky->info.sky_illuminance / sky->info.sun_illuminance;
            }
        }
        else {
            GC_WARN("There is no sky named {}. Does content/textures need to be packaged again?", sky_name);
        }
        const gc::Entity sky = world.createEntity("sky"_name);
        world.addComponent<gc::SkyboxComponent>(sky).setSky(sky_name).setIlluminance(sky_illuminance);
        GC_INFO("The sky {} gives {:.0f} lux", sky_name, sky_illuminance);

        if (!has_ambient_light) {
            // An ambient light of the same strength. It is only drawn while the sky isn't lighting the world: when that is
            // turned off (the I key, or the debug UI's Render menu), to compare the two.
            world.addComponent<gc::LightComponent>(sky).setType(gc::LightType::AMBIENT).setIntensity(sky_illuminance);
        }
    }
    const float exposure_ev100 = std::log2(illuminance / 3.0f); // see CameraComponent::setExposure()
    GC_INFO("The world's lights give about {} lux, so the camera's exposure is EV {:.1f}", illuminance, exposure_ev100);

    // The player, with a point light on the camera (see FlyCameraSystem).
    // FlyCameraSystem moves it to a sensible place on the first frame.
    const gc::Entity player = world.createEntity("player"_name);
    world.addComponent<gc::CameraComponent>(player).setFOV(glm::radians(70.0f)).setNearPlane(0.05f).setActive(true).setExposure(exposure_ev100);
    addHeadlamp(world, player);
    world.addComponent<FlyCameraComponent>(player);
}

// Optional arguments name the prefab and the sky to load instead: temple_explorer [PREFAB] [SKY]
int main(int argc, char* argv[])
{
    const std::string prefab_name = (argc >= 2) ? argv[1] : "temple.glb";
    const std::string sky_name = (argc >= 3) ? argv[2] : "mpumalanga_veld_puresky_8k.hdr";

    gc::AppInitOptions init_options{};
    init_options.name = "temple_explorer";
    init_options.author = "bailwillharr";
    init_options.version = "v0.1.0";

    gc::App::initialise(init_options);
    gc::App& app = gc::App::instance();

    app.renderBackend().setSyncMode(gc::RenderSyncMode::VSYNC_ON_DOUBLE_BUFFERED);
    createPipelines(app);

    buildWorld(app, gc::Name(prefab_name), sky_name);

    app.window().setTitle("Temple explorer");
    app.window().setIsResizable(true);
    app.window().setMouseCaptured(true);
    app.window().setWindowVisibility(true);

    app.run();

    gc::App::shutdown();

    return EXIT_SUCCESS;
}
