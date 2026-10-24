#include "game.h"

#include <cmath>
#include <cstdlib>

#include <algorithm>
#include <charconv>
#include <random>
#include <string_view>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <gclog/gclog.h>

#include <gamecore/gc_abort.h>
#include <gamecore/gc_app.h>
#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_camera_system.h>
#include <gamecore/gc_collider_component.h>
#include <gamecore/gc_collision_system.h>
#include <gamecore/gc_content.h>
#include <gamecore/gc_debug_ui.h>
#include <gamecore/gc_fog_component.h>
#include <gamecore/gc_light_component.h>
#include <gamecore/gc_light_system.h>
#include <gamecore/gc_name.h>
#include <gamecore/gc_render_backend.h>
#include <gamecore/gc_render_system.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_resource_manager.h>
#include <gamecore/gc_skybox_component.h>
#include <gamecore/gc_terrain.h>
#include <gamecore/gc_terrain_component.h>
#include <gamecore/gc_terrain_system.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_ui.h>
#include <gamecore/gc_ui_component.h>
#include <gamecore/gc_ui_system.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_world.h>

#include "beacons.h"
#include "components.h"
#include "content_names.h"
#include "explorer.h"
#include "flythrough.h"
#include "hud.h"

using namespace gc::literals;

// The light. Mars gets 43% of the sunlight that Earth does, and dust in the air takes some of it and spreads it over the sky
static constexpr float SUN_ELEVATION_DEGREES = 32.0f; // the same as the sky's sun (see contentgen)
static constexpr float SUN_AZIMUTH_DEGREES = 250.0f;  // from the west south west, across the fissure, which runs north to south
static constexpr float SUN_ILLUMINANCE = 32000.0f;    // lux, on a surface facing it
static constexpr float SKY_ILLUMINANCE = 12000.0f;    // lux, on the ground, from the rest of the sky
static constexpr float CAMERA_EXPOSURE_EV100 = 13.6f;
// Haze: the far side of the scan, 10 km away, is mostly hidden by it, and so is its edge. The dust thins out over about 11 km of
// height
static constexpr float FOG_DENSITY = 1.0f / 6000.0f;
static constexpr float FOG_HEIGHT_FALLOFF = 1.0f / 11000.0f;

static bool parseNumber(std::string_view text, float& out)
{
    const char* const last = text.data() + text.size();
    return !text.empty() && std::from_chars(text.data(), last, out).ptr == last;
}

Options parseCommandLine(std::span<const char* const> args)
{
    Options options{};
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg(args[i]);
        const bool has_value = (i + 1 < args.size()) && !std::string_view(args[i + 1]).starts_with("--");
        const std::string_view value = has_value ? std::string_view(args[i + 1]) : std::string_view{};
        float number{};

        if (arg.starts_with("syncmode=")) {
            float sync_mode{};
            if (parseNumber(arg.substr(9), sync_mode) && sync_mode >= 0.0f && sync_mode < 4.0f) {
                options.render_sync_mode = static_cast<int>(sync_mode);
            }
        }
        else if (arg == "--flythrough") {
            options.flythrough_seconds = 60.0f;
            if (has_value && parseNumber(value, number) && number > 0.0f) {
                options.flythrough_seconds = number;
                ++i;
            }
        }
        else if (arg == "--detail" && has_value && parseNumber(value, number) && number > 0.0f) {
            options.detail_distance = number;
            ++i;
        }
        else if (arg == "--cache" && has_value && parseNumber(value, number) && number >= 64.0f) {
            options.cache_slots = static_cast<uint32_t>(number);
            ++i;
        }
        else if (arg == "--fly") {
            options.start_flying = true;
        }
        else if (arg == "--levels") {
            options.show_levels = true;
        }
        else if (arg == "--spawn" && i + 2 < args.size()) {
            float x{}, y{};
            if (parseNumber(args[i + 1], x) && parseNumber(args[i + 2], y)) {
                options.spawn = glm::vec2{x, y};
            }
            i += 2;
        }
        else if (arg == "--log" && has_value) {
            options.log_file = value;
            ++i;
        }
        else {
            GC_ERROR("Unknown option: {}", arg);
        }
    }
    return options;
}

Sites findSites(const gc::ResourceTerrain& terrain, std::optional<glm::vec2> spawn)
{
    // The edges of a scan are often wrong, so places are only chosen where there is data all around, well in from the edge
    constexpr float MARGIN = 80.0f;
    const auto is_data = [&](float x, float y) {
        const auto node = terrain.findNode(x, y);
        return node && node->level == 0; // (level 0 is only stored where there is data)
    };
    const auto well_inside = [&](float x, float y) {
        return is_data(x, y) && is_data(x - MARGIN, y) && is_data(x + MARGIN, y) && is_data(x, y - MARGIN) && is_data(x, y + MARGIN);
    };
    const auto ground = [&](float x, float y) { return glm::vec3{x, y, terrain.getHeight(x, y).value_or(0.0f)}; };

    const glm::vec2 data_min = terrain.getDataMin();
    const glm::vec2 data_max = terrain.getDataMax();
    Sites sites{};
    bool found = false;
    constexpr float STEP = 20.0f;
    for (float y = data_min.y; y <= data_max.y; y += STEP) {
        for (float x = data_min.x; x <= data_max.x; x += STEP) {
            if (!well_inside(x, y)) {
                continue;
            }
            const glm::vec3 p = ground(x, y);
            if (!found || p.z < sites.deepest.z) {
                sites.deepest = p;
            }
            if (!found || p.z > sites.highest.z) {
                sites.highest = p;
            }
            found = true;
        }
    }
    if (!found) {
        const glm::vec2 centre = 0.5f * (data_min + data_max);
        sites.deepest = sites.highest = ground(centre.x, centre.y);
    }

    // Others, spread out over the scan
    std::mt19937 random{1975};
    std::uniform_real_distribution<float> random_x(data_min.x, data_max.x);
    std::uniform_real_distribution<float> random_y(data_min.y, data_max.y);
    constexpr size_t OTHER_COUNT = 6;
    constexpr float MIN_APART = 1300.0f;
    for (int attempt = 0; attempt < 5000 && sites.others.size() < OTHER_COUNT; ++attempt) {
        const float x = random_x(random);
        const float y = random_y(random);
        if (!well_inside(x, y)) {
            continue;
        }
        const glm::vec2 p{x, y};
        bool apart = glm::distance(p, glm::vec2(sites.deepest)) > MIN_APART && glm::distance(p, glm::vec2(sites.highest)) > MIN_APART;
        for (const glm::vec3& other : sites.others) {
            apart = apart && glm::distance(p, glm::vec2(other)) > MIN_APART;
        }
        if (apart) {
            sites.others.push_back(ground(x, y));
        }
    }

    // Start on the highest ground near the bottom of the fissure, looking down into it
    if (spawn) {
        sites.spawn = ground(spawn->x, spawn->y);
    }
    else {
        constexpr float RING = 260.0f;
        bool spawn_found = false;
        for (int i = 0; i < 72; ++i) {
            const float angle = glm::two_pi<float>() * static_cast<float>(i) / 72.0f;
            const glm::vec2 p = glm::vec2(sites.deepest) + RING * glm::vec2{std::cos(angle), std::sin(angle)};
            if (!is_data(p.x, p.y)) {
                continue;
            }
            const glm::vec3 g = ground(p.x, p.y);
            if (!spawn_found || g.z > sites.spawn.z) {
                sites.spawn = g;
                spawn_found = true;
            }
        }
        if (!spawn_found) {
            sites.spawn = sites.deepest;
        }
    }
    const glm::vec2 to_deepest = glm::vec2(sites.deepest - sites.spawn);
    sites.spawn_yaw = (glm::length(to_deepest) > 1.0f) ? std::atan2(to_deepest.x, to_deepest.y) : 0.0f;
    sites.spawn.z += 0.05f;
    return sites;
}

static void createPipelines(gc::App& app)
{
    gc::Content& content = app.content();
    const auto frag = content.findAsset("pbr.frag"_name);
    const auto single_draw_vert = content.findAsset("pbr_single_draw.vert"_name);
    const auto instanced_vert = content.findAsset("pbr_instanced.vert"_name);
    const auto sky_vert = content.findAsset("sky.vert"_name);
    const auto sky_frag = content.findAsset("sky.frag"_name);
    const auto terrain_vert = content.findAsset("terrain.vert"_name);
    const auto terrain_frag = content.findAsset("terrain.frag"_name);
    for (const auto* const shader : {&frag, &single_draw_vert, &instanced_vert, &sky_vert, &sky_frag, &terrain_vert, &terrain_frag}) {
        if (shader->data.empty() || shader->type != gcpak::GcpakAssetType::SPIRV_SHADER) {
            gc::abortGame("Failed to find the shaders. Does content/shaders.gcpak need to be packaged again? (see tools/gcpak_editor/README)");
        }
    }
    app.renderBackend().setWorldShaders(single_draw_vert.data, instanced_vert.data, frag.data);
    app.renderBackend().setSkyShaders(sky_vert.data, sky_frag.data);
    app.renderBackend().setTerrainShaders(terrain_vert.data, terrain_frag.data);
}

int buildAndStartGame(gc::App& app, const Options& options)
{
    gc::World& world = app.world();

    if (options.render_sync_mode.has_value()) {
        app.renderBackend().setSyncMode(static_cast<gc::RenderSyncMode>(options.render_sync_mode.value()));
    }
    else {
        app.renderBackend().setSyncMode(gc::RenderSyncMode::VSYNC_ON_DOUBLE_BUFFERED);
    }
    createPipelines(app);
    app.window().setTitle("Mars Fissure");
    app.window().setIsResizable(true);
    app.window().setMouseCaptured(true);

    // The terrain is found by name, in whichever package file it is in (mars_terrain.gcpak)
    const gc::ResourceTerrain* const terrain = app.resourceManager().get<gc::ResourceTerrain>(gc::Name(TERRAIN_ASSET));
    if (!terrain) {
        gc::abortGame("Failed to load the terrain {}. content/mars_terrain.gcpak is baked from the HiRISE DTM by the mars_fissure_terrain target "
                      "(see mars_fissure/README)",
                      TERRAIN_ASSET);
    }
    const Sites sites = findSites(*terrain, options.spawn);
    GC_INFO("The deepest point is at ({:.0f}, {:.0f}), {:.1f} m. The highest is at ({:.0f}, {:.0f}), {:.1f} m. Starting at ({:.0f}, {:.0f})",
            sites.deepest.x, sites.deepest.y, sites.deepest.z, sites.highest.x, sites.highest.y, sites.highest.z, sites.spawn.x, sites.spawn.y);

    world.registerComponent<gc::RenderableComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::CameraComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::LightComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::SkyboxComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::FogComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::ColliderComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::TerrainComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<ExplorerComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<BeaconComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::UIComponent, gc::ComponentArrayType::SPARSE>(); // the HUD

    // The terrain, at 1:1: its corner (the south west of the scan) at the world's origin, with its heights as the scan has them,
    // in metres above Mars's datum
    {
        const gc::Entity entity = world.createEntity("terrain"_name);
        world.addComponent<gc::TerrainComponent>(entity)
            .setTerrain(gc::Name(TERRAIN_ASSET))
            .setMaterial(gc::Name(GROUND_MATERIAL))
            .setDetailDistance(options.detail_distance);
    }

    // The sun. A directional light shines along its entity's -Z, so +Z is turned to point at the sun
    {
        const float elevation = glm::radians(SUN_ELEVATION_DEGREES);
        const float azimuth = glm::radians(SUN_AZIMUTH_DEGREES);
        const glm::vec3 to_sun{std::cos(elevation) * std::sin(azimuth), std::cos(elevation) * std::cos(azimuth), std::sin(elevation)};
        const glm::vec3 axis = glm::normalize(glm::cross(glm::vec3{0.0f, 0.0f, 1.0f}, to_sun));
        const glm::quat rotation = glm::angleAxis(std::acos(std::clamp(to_sun.z, -1.0f, 1.0f)), axis);
        const gc::Entity sun = world.createEntity("sun"_name, gc::ENTITY_NONE, glm::vec3{0.0f}, rotation);
        world.addComponent<gc::LightComponent>(sun).setType(gc::LightType::DIRECTIONAL).setColor({1.0f, 0.93f, 0.82f}).setIntensity(SUN_ILLUMINANCE);
    }

    // The sky, which also lights everything (and is turned so that its sun is where the light is), and the haze
    {
        const gc::Entity sky = world.createEntity("sky"_name);
        world.addComponent<gc::SkyboxComponent>(sky).setSky(SKY_ASSET).setIlluminance(SKY_ILLUMINANCE).setFollowSun(true);
        // (an ambient light for until the sky has been uploaded)
        world.addComponent<gc::LightComponent>(sky).setType(gc::LightType::AMBIENT).setColor({0.9f, 0.7f, 0.55f}).setIntensity(SKY_ILLUMINANCE);
        const gc::Entity haze = world.createEntity("haze"_name, gc::ENTITY_NONE, glm::vec3{0.0f, 0.0f, terrain->header.min_height});
        world.addComponent<gc::FogComponent>(haze).setDensity(FOG_DENSITY).setHeightFalloff(FOG_HEIGHT_FALLOFF).setColor({0.8f, 0.6f, 0.45f});
    }

    // The explorer, whose eyes are the camera
    const gc::Entity explorer = world.createEntity("explorer"_name, gc::ENTITY_NONE, sites.spawn + glm::vec3{0.0f, 0.0f, EXPLORER_EYE_HEIGHT},
                                                   ExplorerSystem::makeLookRotation(sites.spawn_yaw, -0.1f));
    {
        ExplorerComponent& component = world.addComponent<ExplorerComponent>(explorer);
        component.yaw = sites.spawn_yaw;
        component.pitch = -0.1f;
        component.flying = options.start_flying;
        world.addComponent<gc::CameraComponent>(explorer)
            .setFOV(glm::radians(70.0f))
            .setNearPlane(0.1f)
            .setActive(true)
            .setExposure(CAMERA_EXPOSURE_EV100);
    }

    // Systems are updated in the order they are registered. The CollisionSystem goes before the systems that ask it questions.
    // The TerrainSystem chooses what to draw for the camera, so it goes after whatever moves the camera.
    world.registerSystem<gc::CollisionSystem>(app.resourceManager());
    gc::CollisionSystem& collision = world.getSystem<gc::CollisionSystem>();
    const bool flythrough = options.flythrough_seconds > 0.0f;
    if (!flythrough) {
        world.registerSystem<ExplorerSystem>(collision, *terrain, explorer, sites.spawn, sites.spawn_yaw);
    }
    world.registerSystem<BeaconSystem>(app.resourceManager(), sites, explorer);
    world.registerSystem<gc::RenderSystem>(app.resourceManager(), app.renderBackend());
    world.registerSystem<gc::CameraSystem>();
    world.registerSystem<gc::LightSystem>();
    world.registerSystem<gc::TerrainSystem>(app.resourceManager(), app.renderBackend(), options.cache_slots);
    gc::TerrainSystem& terrain_system = world.getSystem<gc::TerrainSystem>();
    terrain_system.getSettings().show_levels = options.show_levels;
    if (flythrough) {
        world.registerSystem<FlythroughSystem>(app, *terrain, terrain_system, explorer, sites, options.flythrough_seconds);
    }
    world.registerSystem<HudSystem>(app.ui(), app.debugUI(), terrain_system, *terrain, flythrough ? nullptr : &world.getSystem<ExplorerSystem>(),
                                    world.getSystem<BeaconSystem>(), explorer);
    world.registerSystem<gc::UISystem>(app.ui()); // (after the HUD, which makes the data model that its document uses)

    app.window().setWindowVisibility(true);
    app.run();
    return EXIT_SUCCESS;
}
