#include "game.h"

#include <cstdlib>

#include <charconv>
#include <string_view>
#include <vector>

#include <gamecore/gc_abort.h>
#include <gamecore/gc_app.h>
#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_camera_system.h>
#include <gamecore/gc_collider_component.h>
#include <gamecore/gc_collision_system.h>
#include <gamecore/gc_content.h>
#include <gamecore/gc_light_component.h>
#include <gamecore/gc_light_system.h>
#include <gamecore/gc_name.h>
#include <gamecore/gc_net.h>
#include <gamecore/gc_prefab.h>
#include <gamecore/gc_render_backend.h>
#include <gamecore/gc_render_system.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_replication.h>
#include <gamecore/gc_resource_manager.h>
#include <gamecore/gc_shadow_map_component.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_world.h>

#include "bot.h"
#include "components.h"
#include "hud.h"
#include "match.h"
#include "player_control.h"
#include "visuals.h"

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
        createGameResources(app.resourceManager());

        app.window().setTitle("Ember Court");
        app.window().setIsResizable(true);
        app.window().setMouseCaptured(true);
    }

    // Every host registers the same components, including the ones it never uses, so that there is only one configuration.
    // It also means that the map's prefab loads the same way everywhere: prefabs find components by name.
    world.registerComponent<gc::RenderableComponent, gc::ComponentArrayType::DENSE>();
    world.registerComponent<gc::CameraComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::LightComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::ShadowMapComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::ColliderComponent, gc::ComponentArrayType::DENSE>(); // nearly everything in the map is solid
    world.registerComponent<SpawnPointComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<PickupSpawnerComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<PatrolPointComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<SpinComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<PlayerComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<ProjectileComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<PickupComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<SentinelComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<CharacterComponent, gc::ComponentArrayType::SPARSE>();
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
    // Replication goes first so that the rest see the latest state from the network, and what happened to it (see getEvents()).
    // The CollisionSystem goes before everything that asks it questions. Hosts without a window need it too: the server uses
    // it to stop bolts at walls, and bots walk on the map like anyone else.
    world.registerSystem<gc::ReplicationSystem>(app.net());
    gc::ReplicationSystem& replication = world.getSystem<gc::ReplicationSystem>();
    world.registerSystem<gc::CollisionSystem>(app.resourceManager());
    gc::CollisionSystem& collision = world.getSystem<gc::CollisionSystem>();
    world.registerSystem<MatchSystem>(app.net(), replication, collision, config);
    MatchSystem& match = world.getSystem<MatchSystem>();
    if (options.bot) {
        world.registerSystem<BotSystem>(replication, collision, match, bot_config);
    }
    else if (!headless) {
        world.registerSystem<PlayerControlSystem>(collision, match);
    }
    if (!headless) {
        world.registerSystem<VisualsSystem>();
        world.registerSystem<HudSystem>(replication, match);
        world.registerSystem<gc::RenderSystem>(app.resourceManager(), app.renderBackend());
        world.registerSystem<gc::CameraSystem>();
        world.registerSystem<gc::LightSystem>();
    }

    // The whole map is one prefab: its geometry, materials, lights, baked shadows, sky, and the places that the game's rules need
    // to know about. It was made by ember_court_mapgen and its shadows were baked by gcpak_editor (see README).
    // Every host loads it for itself, so none of it is sent over the network.
    std::vector<gc::Entity> map_entities{};
    if (gc::loadPrefab(app.content(), MAP_PREFAB, world, gc::ENTITY_NONE, &map_entities) == gc::ENTITY_NONE) {
        gc::abortGame("Failed to load the map. content/ember_court.gcpak is made by building the ember_court_map target (see ember_court/README)");
    }
    GC_INFO("Loaded the map: {} entities", map_entities.size());

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
