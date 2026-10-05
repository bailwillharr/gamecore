#include "game.h"

#include <cstdlib>

#include <array>
#include <charconv>
#include <string_view>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <gamecore/gc_abort.h>
#include <gamecore/gc_app.h>
#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_camera_system.h>
#include <gamecore/gc_content.h>
#include <gamecore/gc_gen_mesh.h>
#include <gamecore/gc_light_component.h>
#include <gamecore/gc_light_system.h>
#include <gamecore/gc_name.h>
#include <gamecore/gc_net.h>
#include <gamecore/gc_render_backend.h>
#include <gamecore/gc_render_system.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_replication.h>
#include <gamecore/gc_resource_manager.h>
#include <gamecore/gc_shadow_map_component.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_world.h>

#include "arena.h"
#include "bot.h"
#include "hud.h"
#include "mouse_move.h"
#include "spin.h"

using namespace gc::literals;

static bool parseNumber(std::string_view text, int& out)
{
    const char* const last = text.data() + text.size();
    return !text.empty() && std::from_chars(text.data(), last, out).ptr == last;
}

static bool parsePort(std::string_view text, uint16_t& out)
{
    int value{};
    if (!parseNumber(text, value) || value < 0 || value > UINT16_MAX) {
        return false;
    }
    out = static_cast<uint16_t>(value);
    return true;
}

Options parseCommandLine(std::span<const char* const> args)
{
    Options options{};
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg(args[i]);
        // some options take a value, which for a few of them is optional
        const bool has_value = (i + 1 < args.size()) && !std::string_view(args[i + 1]).starts_with("--");
        const std::string_view value = has_value ? std::string_view(args[i + 1]) : std::string_view{};
        bool used_value = false;

        if (arg.starts_with("syncmode=")) {
            int sync_mode{};
            if (parseNumber(arg.substr(9), sync_mode) && sync_mode >= 0 && sync_mode < 4) {
                options.render_sync_mode = sync_mode;
            }
        }
        else if (arg == "--host" || arg == "--server") {
            options.mode = (arg == "--host") ? GameMode::HOST : GameMode::DEDICATED_SERVER;
            used_value = has_value && parsePort(value, options.port);
        }
        else if (arg == "--connect" && has_value) {
            options.mode = GameMode::CLIENT;
            options.address = value;
            // address:port, unless it's an IPv6 address (use --port for those)
            const size_t colon = value.find(':');
            if (colon != std::string_view::npos && colon == value.rfind(':') && parsePort(value.substr(colon + 1), options.port)) {
                options.address = value.substr(0, colon);
            }
            used_value = true;
        }
        else if (arg == "--port" && has_value) {
            used_value = parsePort(value, options.port);
        }
        else if (arg == "--bind" && has_value) {
            options.bind_address = value;
            used_value = true;
        }
        else if (arg == "--bot") {
            options.bot = true;
        }
        else if (arg == "--test") {
            options.test = true;
        }
        else if (arg == "--exit-when-empty") {
            options.exit_when_empty = true;
        }
        else if (arg == "--log" && has_value) {
            options.log_file = value;
            used_value = true;
        }
        else if ((arg == "--test-timeout" || arg == "--test-freeze" || arg.starts_with("--sim-")) && has_value) {
            const float number = std::strtof(std::string(value).c_str(), nullptr);
            used_value = true;
            if (arg == "--test-timeout") {
                options.test_timeout = number;
            }
            else if (arg == "--test-freeze") {
                options.test_freeze_time = number;
            }
            else if (arg == "--sim-loss") {
                options.sim.loss_percent = number;
                options.sim.enabled = true;
            }
            else if (arg == "--sim-duplicate") {
                options.sim.duplicate_percent = number;
                options.sim.enabled = true;
            }
            else if (arg == "--sim-latency") {
                options.sim.latency_ms = number;
                options.sim.enabled = true;
            }
            else if (arg == "--sim-jitter") {
                options.sim.jitter_ms = number;
                options.sim.enabled = true;
            }
            else {
                GC_ERROR("Unknown option: {}", arg);
            }
        }
        else {
            GC_ERROR("Unknown option: {}", arg);
        }

        if (used_value) {
            ++i;
        }
    }
    return options;
}

bool isHeadless(const Options& options) { return options.mode == GameMode::DEDICATED_SERVER || options.bot; }

// Lights are in physical units. The sun is low and dim (lux), so that the lamp in the room can be seen next to it.
// The cameras' exposure has to suit it, see CAMERA_EXPOSURE_EV100 in arena.h
static constexpr float SUN_ILLUMINANCE = 400.0f;

static void createPipelines(gc::App& app)
{
    gc::Content& content = app.content();
    const auto frag = content.findAsset("pbr.frag"_name);
    const auto single_draw_vert = content.findAsset("pbr_single_draw.vert"_name);
    const auto instanced_vert = content.findAsset("pbr_instanced.vert"_name);
    for (const auto* const shader : {&frag, &single_draw_vert, &instanced_vert}) {
        if (shader->data.empty() || shader->type != gcpak::GcpakAssetType::SPIRV_SHADER) {
            gc::abortGame("Failed to find shaders");
        }
    }
    app.renderBackend().setWorldShaders(single_draw_vert.data, instanced_vert.data, frag.data);
}

// The parts of the world that never change. Every host makes its own copy, so none of it needs replicating.
// It's only scenery (nothing collides with it) so hosts without a window don't need it at all.
static void createScenery(gc::App& app)
{
    gc::ResourceManager& resource_manager = app.resourceManager();
    gc::World& world = app.world();

    {
        gc::ResourceMaterial material{};
        material.base_color_texture = "bricks-mortar-albedo.png"_name;
        material.orm_texture = "bricks-mortar-orm.png"_name;
        material.normal_texture = "bricks-mortar-normal.png"_name;
        resource_manager.add<gc::ResourceMaterial>(std::move(material), "bricks-mortar"_name);
    }
    {
        gc::ResourceMaterial material{};
        material.base_color_texture = "laminate-flooring-brown_albedo.png"_name;
        material.orm_texture = "laminate-flooring-brown_orm.png"_name;
        material.normal_texture = "laminate-flooring-brown_normal.png"_name;
        resource_manager.add<gc::ResourceMaterial>(std::move(material), "laminate-flooring-brown"_name);
    }
    {
        gc::ResourceMaterial material{};
        material.base_color_texture = "uvcheck.png"_name;
        resource_manager.add<gc::ResourceMaterial>(std::move(material), "testmat"_name);
    }
    resource_manager.add<gc::ResourceMesh>(gc::genPlaneMesh(100.0f, 100.0f), "floor"_name);
    resource_manager.add<gc::ResourceMesh>(gc::genPlaneMesh(10.0f, 4.0f), "wall1"_name);
    resource_manager.add<gc::ResourceMesh>(gc::genSphereMesh(50), "ball"_name);
    resource_manager.add<gc::ResourceMesh>(gc::genCubeMesh(), "cube"_name);

    {
        // Lights shine along their -Z axis, as cameras look. This one shines down from above +X +Y.
        const auto sun = world.createEntity("sun"_name);
        world.getComponent<gc::TransformComponent>(sun)->setRotation(
            glm::quatLookAt(glm::normalize(glm::vec3{-1.0f, -1.0f, -1.0f}), glm::vec3{0.0f, 0.0f, 1.0f}));
        world.addComponent<gc::LightComponent>(sun).setType(gc::LightType::DIRECTIONAL).setIntensity(SUN_ILLUMINANCE);
    }
    {
        // the sky: a tenth of the sun
        const auto sky = world.createEntity("sky"_name);
        world.addComponent<gc::LightComponent>(sky).setType(gc::LightType::AMBIENT).setIntensity(0.1f * SUN_ILLUMINANCE);
    }
    {
        // a lamp in the room in the middle of the arena
        const auto light = world.createEntity("light"_name, gc::ENTITY_NONE, {0.0f, 0.0f, 3.0f});
        world.addComponent<gc::LightComponent>(light).setColor({1.0f, 0.8f, 0.5f}).setIntensity(1000.0f).setRange(12.0f); // candela
    }
    {
        const auto floor = world.createEntity("floor"_name);
        world.getComponent<gc::TransformComponent>(floor)->setScale({100.0f, 100.0f, 1.0f});
        world.addComponent<gc::RenderableComponent>(floor).setMesh("floor"_name).setMaterial("laminate-flooring-brown"_name);
    }

    // a room in the middle of the arena. Each wall has two sides.
    struct Wall {
        gc::Name name;
        glm::vec3 position;
        glm::quat rotation;
    };
    const float r = glm::one_over_root_two<float>();
    const std::array<Wall, 6> walls{{
        {"wall1"_name, {-5.0f, 0.0f, 2.0f}, glm::quat(0.5f, 0.5f, 0.5f, 0.5f)},
        {"wall2"_name, {5.0f, 0.0f, 2.0f}, glm::quat(0.5f, 0.5f, -0.5f, -0.5f)},
        {"wall3"_name, {0.0f, -5.0f, 2.0f}, glm::quat(0.0f, 0.0f, -r, -r)},
        {"wall4"_name, {-5.0f, 0.0f, 2.0f}, glm::quat(0.5f, 0.5f, -0.5f, -0.5f)},
        {"wall5"_name, {5.0f, 0.0f, 2.0f}, glm::quat(0.5f, 0.5f, 0.5f, 0.5f)},
        {"wall6"_name, {0.0f, -5.0f, 2.0f}, glm::quat(-r, -r, 0.0f, 0.0f)},
    }};
    for (const Wall& wall : walls) {
        const auto entity = world.createEntity(wall.name, gc::ENTITY_NONE, wall.position, wall.rotation, {10.0f, 4.0f, 1.0f});
        world.addComponent<gc::RenderableComponent>(entity).setMaterial("bricks-mortar"_name).setMesh("wall1"_name);
    }
    {
        const auto roof = world.createEntity("roof"_name);
        world.getComponent<gc::TransformComponent>(roof)->setPosition(0, 0, 4).setRotation(0, -1, 0, 0).setScale(10, 10, 1);
        world.addComponent<gc::RenderableComponent>(roof).setMesh("floor"_name);
    }
}

// returns false on failure
static bool startNetworking(gc::App& app, const Options& options)
{
    gc::Net& net = app.net();
    net.setSimConfig(options.sim);

    switch (options.mode) {
    case GameMode::OFFLINE:
        return true;
    case GameMode::HOST:
    case GameMode::DEDICATED_SERVER: {
        asio::ip::address address = asio::ip::address_v6(); // default :: (accepts ipv4 connections too)
        if (!options.bind_address.empty()) {
            asio::error_code ec{};
            address = asio::ip::make_address(options.bind_address, ec);
            if (ec) {
                GC_ERROR("Invalid address to listen on: {}", options.bind_address);
                return false;
            }
        }
        return net.startServer(asio::ip::udp::endpoint(address, options.port));
    }
    case GameMode::CLIENT: {
        const auto endpoint = net.resolve(options.address, std::to_string(options.port));
        if (!endpoint) {
            GC_ERROR("Failed to resolve server address: {}", options.address);
            return false;
        }
        return net.connectToServer(*endpoint);
    }
    }
    return false;
}

int buildAndStartGame(gc::App& app, const Options& options)
{
    const bool headless = isHeadless(options);
    gc::World& world = app.world();

    if (!headless) {
        if (options.render_sync_mode.has_value()) {
            app.renderBackend().setSyncMode(static_cast<gc::RenderSyncMode>(options.render_sync_mode.value()));
        }
        else {
            // On Windows/NVIDIA, TRIPLE_BUFFERED gives horrible latency and TRIPLE_BUFFERED_UNTHROTTLED doesn't work properly so use
            // double buffering instead
            app.renderBackend().setSyncMode(gc::RenderSyncMode::VSYNC_ON_DOUBLE_BUFFERED);
        }
        createPipelines(app);

        app.window().setTitle("Gamecore multiplayer test");
        app.window().setIsResizable(true);
        app.window().setMouseCaptured(true);
    }

    // Every host registers the same components, including the ones it never uses, so that there is only one configuration
    world.registerComponent<gc::RenderableComponent, gc::ComponentArrayType::DENSE>();
    world.registerComponent<gc::CameraComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::LightComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::ShadowMapComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<SpinComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<MouseMoveComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<PlayerComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<ProjectileComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<PickupComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<OrbiterComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<PlayerModelComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<BotComponent, gc::ComponentArrayType::SPARSE>();

    GameConfig config{};
    config.headless = headless;
    config.bot = options.bot;
    config.spawn_local_player = !headless || options.bot; // a bot can host too
    config.exit_when_empty = options.exit_when_empty;
    config.freeze_time = options.test_freeze_time;

    int exit_code = EXIT_SUCCESS;

    BotConfig bot_config{};
    bot_config.test = options.test;
    bot_config.test_timeout = options.test_timeout;
    bot_config.exit_code = &exit_code;

    // Systems are updated in the order they are registered.
    // Replication goes first so that the rest see the latest state from the network, and what happened to it (see getEvents())
    world.registerSystem<gc::ReplicationSystem>(app.net());
    gc::ReplicationSystem& replication = world.getSystem<gc::ReplicationSystem>();
    if (!headless) {
        world.registerSystem<MouseMoveSystem>();
    }
    world.registerSystem<ArenaSystem>(app.net(), replication, config);
    if (options.bot) {
        world.registerSystem<BotSystem>(replication, world.getSystem<ArenaSystem>(), bot_config);
    }
    world.registerSystem<SpinSystem>();
    if (!headless) {
        world.registerSystem<HudSystem>(replication);
        world.registerSystem<gc::RenderSystem>(app.resourceManager(), app.renderBackend());
        world.registerSystem<gc::CameraSystem>();
        world.registerSystem<gc::LightSystem>();

        createScenery(app);
    }

    if (!startNetworking(app, options)) {
        if (headless) {
            return EXIT_FAILURE; // nothing useful can happen without a window or a connection
        }
        GC_ERROR("Failed to start networking, continuing offline");
    }

    if (!headless) {
        app.window().setWindowVisibility(true);
    }

    app.run();

    return exit_code;
}
