#include "match.h"

#include <cmath>

#include <algorithm>
#include <array>
#include <limits>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <tracy/Tracy.hpp>

#include <gamecore/gc_app.h>
#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_collision_system.h>
#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_gen_mesh.h>
#include <gamecore/gc_light_component.h>
#include <gamecore/gc_net.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_replication.h>
#include <gamecore/gc_resource_manager.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_world.h>

#include "bot.h"
#include "character.h"

static constexpr gc::Name MESSAGE_FIRE = gc::Name::createConstexpr("ec_fire");       // client -> server: direction (3 floats)
static constexpr gc::Name MESSAGE_RESPAWN = gc::Name::createConstexpr("ec_respawn"); // server -> client: where its feet go (3 floats), yaw
static constexpr gc::Name MESSAGE_HIT = gc::Name::createConstexpr("ec_hit");         // server -> client: a bolt it threw hit something
static constexpr gc::Name MESSAGE_KILL = gc::Name::createConstexpr("ec_kill");       // server -> clients: killer, victim (peer IDs)

static constexpr gc::Name MESH_PLAYER = gc::Name::createConstexpr("shrek.obj");
static constexpr gc::Name MESH_SPHERE = gc::Name::createConstexpr("ember_court/runtime_sphere");
static constexpr gc::Name MESH_CUBE = gc::Name::createConstexpr("ember_court/runtime_cube");
static constexpr gc::Name MATERIAL_BOLT = gc::Name::createConstexpr("ember_court/runtime_bolt");
static constexpr gc::Name MATERIAL_EMBER = gc::Name::createConstexpr("ember_court/runtime_ember");
static constexpr gc::Name MATERIAL_HEALTH = gc::Name::createConstexpr("ember_court/runtime_health");
static constexpr gc::Name MATERIAL_SENTINEL = gc::Name::createConstexpr("ember_court/runtime_sentinel");
static constexpr gc::Name MATERIAL_SENTINEL_EYE = gc::Name::createConstexpr("ember_court/runtime_sentinel_eye");
static constexpr std::array<gc::Name, 6> PLAYER_MATERIALS{
    gc::Name::createConstexpr("ember_court/runtime_player0"), gc::Name::createConstexpr("ember_court/runtime_player1"),
    gc::Name::createConstexpr("ember_court/runtime_player2"), gc::Name::createConstexpr("ember_court/runtime_player3"),
    gc::Name::createConstexpr("ember_court/runtime_player4"), gc::Name::createConstexpr("ember_court/runtime_player5"),
};
static constexpr gc::Name ENTITY_OVERVIEW_CAMERA = gc::Name::createConstexpr("overview_camera");

static constexpr float PLAYER_HIT_RADIUS = 0.6f;
static constexpr float PLAYER_MODEL_SCALE = 0.36f;
static constexpr float SENTINEL_RADIUS = 0.6f;
static constexpr float BOLT_RADIUS = 0.12f;
static constexpr float BOLT_MUZZLE_DISTANCE = 0.6f; // bolts start this far in front of the eyes, so that they can be seen leaving
static constexpr float PICKUP_RADIUS = 1.3f;
static constexpr size_t VEC3_SIZE = 3 * sizeof(float);
static constexpr size_t RESPAWN_SIZE = 4 * sizeof(float);
static constexpr size_t KILL_SIZE = 2 * sizeof(uint32_t);
static constexpr size_t MAX_KILL_FEED = 5;

static bool isFinite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

static void writeVec3(gc::ByteWriter& writer, const glm::vec3& v)
{
    writer.writeF32(v.x);
    writer.writeF32(v.y);
    writer.writeF32(v.z);
}

static glm::vec3 readVec3(gc::ByteReader& reader)
{
    glm::vec3 v{};
    v.x = reader.readF32();
    v.y = reader.readF32();
    v.z = reader.readF32();
    return v;
}

static float distanceToSegment(const glm::vec3& point, const glm::vec3& a, const glm::vec3& b)
{
    const glm::vec3 ab = b - a;
    const float length2 = glm::dot(ab, ab);
    const float t = (length2 > 0.0f) ? std::clamp(glm::dot(point - a, ab) / length2, 0.0f, 1.0f) : 0.0f;
    return glm::distance(point, a + ab * t);
}

// the middle of a player's body
static glm::vec3 getPlayerCentre(const glm::vec3& eye_position) { return eye_position - glm::vec3(0.0f, 0.0f, CHARACTER_EYE_HEIGHT - 0.5f * CHARACTER_HEIGHT); }

// Replicated entities have no appearance of their own, they get a child entity to render (which isn't replicated)
static gc::Entity addModel(gc::World& world, gc::Entity parent, gc::Name mesh, gc::Name material, float scale, const glm::vec3& position = glm::vec3{0.0f})
{
    const gc::Entity model = world.createEntity(gc::Name::createConstexpr("model"), parent, position);
    world.getComponent<gc::TransformComponent>(model)->setScale(scale);
    world.addComponent<gc::RenderableComponent>(model).setMesh(mesh).setMaterial(material);
    return model;
}

static gc::ResourceMaterial makeGlowingMaterial(const glm::vec3& emissive)
{
    gc::ResourceMaterial material{};
    material.base_color = glm::vec4{0.02f, 0.02f, 0.02f, 1.0f};
    material.roughness = 1.0f;
    material.emissive = emissive;
    return material;
}

void createGameResources(gc::ResourceManager& resource_manager)
{
    // Resources don't have to come from a .gcpak file: these are made here and added under names of their own.
    resource_manager.add<gc::ResourceMesh>(gc::genSphereMesh(24), MESH_SPHERE);
    resource_manager.add<gc::ResourceMesh>(gc::genCubeMesh(), MESH_CUBE);

    // (emission isn't in physical units: 1 is about as bright as a lit white surface. See ResourceMaterial)
    resource_manager.add<gc::ResourceMaterial>(makeGlowingMaterial({1.0f, 0.45f, 0.08f}), MATERIAL_BOLT);
    resource_manager.add<gc::ResourceMaterial>(makeGlowingMaterial({1.0f, 0.62f, 0.12f}), MATERIAL_EMBER);
    resource_manager.add<gc::ResourceMaterial>(makeGlowingMaterial({0.15f, 0.9f, 0.3f}), MATERIAL_HEALTH);
    resource_manager.add<gc::ResourceMaterial>(makeGlowingMaterial({1.0f, 0.08f, 0.05f}), MATERIAL_SENTINEL_EYE);
    {
        gc::ResourceMaterial material{};
        material.base_color = glm::vec4{0.35f, 0.36f, 0.4f, 1.0f};
        material.roughness = 0.3f;
        material.metallic = 1.0f;
        resource_manager.add<gc::ResourceMaterial>(std::move(material), MATERIAL_SENTINEL);
    }
    // one color for each player, so that they can be told apart
    constexpr std::array<glm::vec3, PLAYER_MATERIALS.size()> PLAYER_COLORS{{
        {0.85f, 0.2f, 0.15f},
        {0.15f, 0.4f, 0.85f},
        {0.2f, 0.7f, 0.25f},
        {0.85f, 0.7f, 0.15f},
        {0.65f, 0.25f, 0.8f},
        {0.15f, 0.7f, 0.75f},
    }};
    for (size_t i = 0; i < PLAYER_MATERIALS.size(); ++i) {
        gc::ResourceMaterial material{};
        material.base_color = glm::vec4{PLAYER_COLORS[i], 1.0f};
        material.roughness = 0.6f;
        material.emissive = PLAYER_COLORS[i] * 0.08f; // so that they can be made out in the shade
        resource_manager.add<gc::ResourceMaterial>(std::move(material), PLAYER_MATERIALS[i]);
    }
}

MatchSystem::MatchSystem(gc::World& world, gc::Net& net, gc::ReplicationSystem& replication, gc::CollisionSystem& collision, const GameConfig& config)
    : gc::System(world), m_net(net), m_replication(replication), m_collision(collision), m_config(config), m_rng(std::random_device{}())
{
    m_replication.registerComponent<PlayerComponent>(gc::ReplicationAuthority::SERVER);

    // An archetype's callback is run on every host when one of its entities appears there, and adds what that host needs.
    {
        gc::ReplicationArchetype archetype{};
        archetype.transform_authority = gc::ReplicationAuthority::OWNER; // the owning client moves its own player
        archetype.on_spawn = [config](gc::World& world, gc::Entity entity, const gc::ReplicationSpawnInfo& info) {
            world.addComponent<PlayerComponent>(entity);
            if (info.locally_owned) {
                world.addComponent<CharacterComponent>(entity);
                if (config.bot) {
                    world.addComponent<BotComponent>(entity);
                }
                else if (!config.headless) {
                    world.addComponent<gc::CameraComponent>(entity)
                        .setFOV(glm::radians(60.0f))
                        .setNearPlane(0.05f)
                        .setActive(true)
                        .setExposure(CAMERA_EXPOSURE_EV100);
                }
            }
            else if (!config.headless) {
                const gc::Entity model = addModel(world, entity, MESH_PLAYER, PLAYER_MATERIALS[info.owner % PLAYER_MATERIALS.size()], PLAYER_MODEL_SCALE);
                world.addComponent<PlayerModelComponent>(entity).model = model;
            }
        };
        m_replication.registerArchetype(ARCHETYPE_PLAYER, std::move(archetype));
    }
    {
        gc::ReplicationArchetype archetype{};
        archetype.transform_authority = gc::ReplicationAuthority::SERVER;
        archetype.on_spawn = [config](gc::World& world, gc::Entity entity, const gc::ReplicationSpawnInfo& info) {
            if (info.has_authority) {
                world.addComponent<ProjectileComponent>(entity);
            }
            if (!config.headless) {
                addModel(world, entity, MESH_SPHERE, MATERIAL_BOLT, BOLT_RADIUS);
                // a bolt lights what it flies past
                world.addComponent<gc::LightComponent>(entity).setColor({1.0f, 0.5f, 0.15f}).setIntensity(350.0f).setRange(8.0f);
            }
        };
        m_replication.registerArchetype(ARCHETYPE_BOLT, std::move(archetype));
    }
    for (const PickupKind kind : {PickupKind::EMBER, PickupKind::HEALTH}) {
        gc::ReplicationArchetype archetype{};
        archetype.transform_authority = gc::ReplicationAuthority::SERVER;
        archetype.interpolate_transform = false; // they don't move
        archetype.on_spawn = [config, kind](gc::World& world, gc::Entity entity, const gc::ReplicationSpawnInfo& info) {
            if (info.has_authority) {
                world.addComponent<PickupComponent>(entity).kind = kind;
            }
            if (!config.headless) {
                const bool ember = (kind == PickupKind::EMBER);
                // Spinning and bobbing is only for show, so each host moves its own model instead of the server replicating it
                const gc::Entity model = addModel(world, entity, MESH_CUBE, ember ? MATERIAL_EMBER : MATERIAL_HEALTH, ember ? 0.3f : 0.4f);
                SpinComponent& spin = world.addComponent<SpinComponent>(model);
                spin.axis = glm::normalize(glm::vec3{0.3f, 0.2f, 1.0f});
                spin.radians_per_second = 2.5f;
                spin.bob_height = 0.12f;
                world.addComponent<gc::LightComponent>(entity)
                    .setColor(ember ? glm::vec3{1.0f, 0.6f, 0.2f} : glm::vec3{0.3f, 1.0f, 0.4f})
                    .setIntensity(60.0f)
                    .setRange(4.0f);
            }
        };
        m_replication.registerArchetype(kind == PickupKind::EMBER ? ARCHETYPE_EMBER : ARCHETYPE_HEALTH, std::move(archetype));
    }
    {
        gc::ReplicationArchetype archetype{};
        archetype.transform_authority = gc::ReplicationAuthority::SERVER;
        archetype.on_spawn = [config](gc::World& world, gc::Entity entity, const gc::ReplicationSpawnInfo& info) {
            if (info.has_authority) {
                world.addComponent<SentinelComponent>(entity);
            }
            if (!config.headless) {
                addModel(world, entity, MESH_SPHERE, MATERIAL_SENTINEL, SENTINEL_RADIUS);
                // Its eye is in front. The entity is turned like a camera, so that is along its -Z axis.
                addModel(world, entity, MESH_SPHERE, MATERIAL_SENTINEL_EYE, 0.22f, {0.0f, 0.0f, -(SENTINEL_RADIUS - 0.08f)});
                world.addComponent<gc::LightComponent>(entity).setColor({1.0f, 0.15f, 0.1f}).setIntensity(150.0f).setRange(7.0f);
            }
        };
        m_replication.registerArchetype(ARCHETYPE_SENTINEL, std::move(archetype));
    }
}

void MatchSystem::onUpdate(gc::FrameState& frame_state)
{
    ZoneScoped;

    m_time += frame_state.delta_time;

    if (!m_map_read) {
        // Done on the first update rather than when the map is loaded, as world positions are only known once the
        // TransformSystem has run.
        readMap();
        m_map_read = true;
    }

    for (const gc::ReplicationEvent& ev : m_replication.getEvents()) {
        switch (ev.kind) {
        case gc::ReplicationEventKind::AUTHORITY_STARTED:
            startWorld();
            break;
        case gc::ReplicationEventKind::PEER_JOINED:
            ++m_connected_peers;
            spawnPlayer(ev.peer);
            break;
        case gc::ReplicationEventKind::PEER_LEFT:
            // the replication system has already despawned the player
            --m_connected_peers;
            m_last_fire_times.erase(ev.peer);
            if (m_config.exit_when_empty && m_connected_peers == 0) {
                GC_INFO("The last client left, exiting");
                gc::App::instance().requestQuit();
            }
            break;
        }
    }

    updateLocalPlayer();
    handleNetEvents(frame_state);

    if (isFrozen()) {
        // Long enough for anything that was lost on the way to be sent again. Lost state is resent within a few ticks, but the
        // last reliable messages (spawns) can take seconds on a very bad connection as their retransmission timeout backs off.
        constexpr double SETTLE_TIME = 5.0;
        if (!m_digest_reported && m_time >= static_cast<double>(m_config.freeze_time) + SETTLE_TIME) {
            m_digest_reported = true;
            GC_INFO("REPLICATION DIGEST {:08x} ({} entities)", m_replication.computeStateDigest(), m_replication.getEntityCount());
            if (!m_replication.hasAuthority()) {
                gc::App::instance().requestQuit();
            }
        }
        return;
    }

    if (m_replication.hasAuthority()) {
        const float delta_time = static_cast<float>(frame_state.delta_time);
        simulateBolts(delta_time);
        simulatePickups();
        simulateSentinels(delta_time);
    }
}

gc::Entity MatchSystem::getLocalPlayer() const { return m_local_player; }

bool MatchSystem::isFrozen() const { return m_config.freeze_time > 0.0f && m_time >= static_cast<double>(m_config.freeze_time); }

bool MatchSystem::requestFire(const glm::vec3& direction)
{
    if (m_local_player == gc::ENTITY_NONE || isFrozen() || m_time - m_last_fire_request_time < static_cast<double>(FIRE_COOLDOWN)) {
        return false;
    }
    m_last_fire_request_time = m_time;

    if (m_replication.hasAuthority()) {
        fire(m_replication.getLocalPeerId(), direction);
    }
    else {
        // Only the server can spawn things. It doesn't matter much if the bolt appears a moment late, but it must appear.
        gc::NetEvent ev{};
        ev.type = MESSAGE_FIRE;
        ev.data.resize(VEC3_SIZE);
        gc::ByteWriter writer(ev.data);
        writeVec3(writer, direction);
        m_net.postEvent(ev, gc::NetDelivery::RELIABLE);
    }
    return true;
}

double MatchSystem::getTime() const { return m_time; }

const std::vector<MatchSystem::KillFeedEntry>& MatchSystem::getKillFeed() const { return m_kill_feed; }

double MatchSystem::getLastHitTime() const { return m_last_hit_time; }

// The map is a prefab that game.cpp loaded into the world. What the game needs to know about it is in its components.
void MatchSystem::readMap()
{
    m_world.forEach<gc::TransformComponent, SpawnPointComponent>([&](gc::Entity, const gc::TransformComponent& t, const SpawnPointComponent& spawn_point) {
        m_spawn_points.push_back(SpawnPoint{t.getWorldPosition(), spawn_point.yaw});
    });

    m_world.forEach<gc::TransformComponent, PickupSpawnerComponent>([&](gc::Entity, const gc::TransformComponent& t, const PickupSpawnerComponent& spawner) {
        PickupSpawner pickup_spawner{};
        pickup_spawner.position = t.getWorldPosition();
        pickup_spawner.kind = spawner.kind;
        pickup_spawner.respawn_time = std::isfinite(spawner.respawn_time) ? std::clamp(spawner.respawn_time, 1.0f, 600.0f) : 10.0f;
        m_pickup_spawners.push_back(pickup_spawner);
    });

    m_world.forEach<gc::TransformComponent, PatrolPointComponent>([&](gc::Entity, const gc::TransformComponent& t, const PatrolPointComponent& point) {
        auto route = std::find_if(m_patrol_routes.begin(), m_patrol_routes.end(), [&](const PatrolRoute& r) { return r.id == point.route; });
        if (route == m_patrol_routes.end()) {
            route = m_patrol_routes.insert(m_patrol_routes.end(), PatrolRoute{point.route, {}});
        }
        route->points.emplace_back(point.order, t.getWorldPosition());
    });
    // Entities aren't visited in any particular order, and every host has to agree on which route is which
    std::sort(m_patrol_routes.begin(), m_patrol_routes.end(), [](const PatrolRoute& a, const PatrolRoute& b) { return a.id < b.id; });
    for (PatrolRoute& route : m_patrol_routes) {
        std::sort(route.points.begin(), route.points.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    }

    if (!m_config.headless) {
        // The map has a camera that looks over the court, used whenever the local host doesn't have a player (e.g. while
        // connecting). Cameras in prefabs aren't active until something makes them.
        m_overview_camera = m_world.findEntity(ENTITY_OVERVIEW_CAMERA);
        if (m_overview_camera == gc::ENTITY_NONE || !m_world.getComponent<gc::CameraComponent>(m_overview_camera)) {
            const glm::vec3 position{0.0f, -30.0f, 15.0f};
            m_overview_camera = m_world.createEntity(ENTITY_OVERVIEW_CAMERA, gc::ENTITY_NONE, position,
                                                     glm::quatLookAt(glm::normalize(-position), glm::vec3{0.0f, 0.0f, 1.0f}));
            m_world.addComponent<gc::CameraComponent>(m_overview_camera).setExposure(CAMERA_EXPOSURE_EV100);
        }
    }

    GC_INFO("The map has {} spawn points, {} pickup spawners and {} patrol routes", m_spawn_points.size(), m_pickup_spawners.size(), m_patrol_routes.size());
}

// The local host has become the authority, and nothing is spawned yet
void MatchSystem::startWorld()
{
    GC_INFO("Starting the match");

    for (uint32_t i = 0; i < m_pickup_spawners.size(); ++i) {
        m_pickup_spawners[i].pickup = gc::ENTITY_NONE;
        spawnPickup(i);
    }

    for (uint32_t i = 0; i < m_patrol_routes.size(); ++i) {
        const PatrolRoute& route = m_patrol_routes[i];
        if (route.points.size() < 2) {
            continue;
        }
        const gc::Entity entity = m_replication.spawn(ARCHETYPE_SENTINEL, gc::NET_PEER_NONE, route.points[0].second);
        if (SentinelComponent* const sentinel = m_world.getComponent<SentinelComponent>(entity); sentinel) {
            sentinel->route = i;
            sentinel->next_point = 1;
        }
    }

    if (m_config.spawn_local_player) {
        spawnPlayer(m_replication.getLocalPeerId());
    }
}

void MatchSystem::spawnPlayer(gc::NetPeerId owner)
{
    const SpawnPoint spawn_point = chooseSpawnPoint();
    m_replication.spawn(ARCHETYPE_PLAYER, owner, spawn_point.feet + glm::vec3{0.0f, 0.0f, CHARACTER_EYE_HEIGHT}, makeLookRotation(spawn_point.yaw, 0.0f));
}

void MatchSystem::spawnPickup(uint32_t spawner_index)
{
    PickupSpawner& spawner = m_pickup_spawners[spawner_index];
    spawner.pickup = m_replication.spawn(spawner.kind == PickupKind::EMBER ? ARCHETYPE_EMBER : ARCHETYPE_HEALTH, gc::NET_PEER_NONE, spawner.position);
    if (PickupComponent* const pickup = m_world.getComponent<PickupComponent>(spawner.pickup); pickup) {
        pickup->spawner = spawner_index;
    }
}

// The spawn point that is furthest from every player, so that nobody appears next to someone who is waiting for them
MatchSystem::SpawnPoint MatchSystem::chooseSpawnPoint()
{
    if (m_spawn_points.empty()) {
        return SpawnPoint{glm::vec3{0.0f, 0.0f, 3.0f}, 0.0f}; // the map has none. Anywhere will have to do
    }
    const std::vector<PlayerInfo> players = getPlayers();
    if (players.empty()) {
        return m_spawn_points[std::uniform_int_distribution<size_t>(0, m_spawn_points.size() - 1)(m_rng)];
    }
    const SpawnPoint* best = nullptr;
    float best_distance = -1.0f;
    for (const SpawnPoint& spawn_point : m_spawn_points) {
        float nearest = std::numeric_limits<float>::max();
        for (const PlayerInfo& player : players) {
            nearest = std::min(nearest, glm::distance(player.position, spawn_point.feet));
        }
        if (nearest > best_distance) {
            best_distance = nearest;
            best = &spawn_point;
        }
    }
    return *best;
}

void MatchSystem::handleNetEvents(const gc::FrameState& frame_state)
{
    const bool has_authority = m_replication.hasAuthority();
    for (const gc::NetEvent& ev : frame_state.net_events) {
        if (ev.kind != gc::NetEventKind::MESSAGE) {
            continue;
        }
        // Clients ask, the server tells. Nothing that arrives is trusted to be the right size or to make sense.
        const bool from_server = !has_authority && ev.peer == gc::NET_PEER_SERVER;
        gc::ByteReader reader(ev.data);
        if (ev.type == MESSAGE_FIRE && has_authority && ev.data.size() == VEC3_SIZE) {
            fire(ev.peer, readVec3(reader));
        }
        else if (ev.type == MESSAGE_RESPAWN && from_server && ev.data.size() == RESPAWN_SIZE) {
            const glm::vec3 feet = readVec3(reader);
            const float yaw = reader.readF32();
            if (isFinite(feet) && std::isfinite(yaw)) {
                placeLocalPlayer(feet, yaw);
            }
        }
        else if (ev.type == MESSAGE_HIT && from_server) {
            m_last_hit_time = m_time;
        }
        else if (ev.type == MESSAGE_KILL && from_server && ev.data.size() == KILL_SIZE) {
            const gc::NetPeerId killer = reader.readU32();
            const gc::NetPeerId victim = reader.readU32();
            m_kill_feed.push_back(KillFeedEntry{killer, victim, m_time});
        }
    }
    if (m_kill_feed.size() > MAX_KILL_FEED) {
        m_kill_feed.erase(m_kill_feed.begin(), m_kill_feed.end() - MAX_KILL_FEED);
    }
}

// Authority only
void MatchSystem::fire(gc::NetPeerId shooter, glm::vec3 direction)
{
    // direction might have come from a client, so it can't be trusted
    const float length = glm::length(direction);
    if (!isFinite(direction) || !std::isfinite(length) || length < 1.0e-3f) {
        return;
    }
    direction /= length;

    // Clients limit how fast they fire themselves. The check here is a bit more lenient to allow for requests bunching up.
    auto& last_fire_time = m_last_fire_times.try_emplace(shooter, -1.0).first->second;
    if (m_time - last_fire_time < static_cast<double>(FIRE_COOLDOWN) * 0.5) {
        return;
    }
    last_fire_time = m_time;

    for (const PlayerInfo& player : getPlayers()) {
        if (player.owner == shooter) {
            // Someone with their face against a wall would otherwise throw the bolt from the other side of it
            if (m_collision.raycast(gc::Ray{player.position, direction}, BOLT_MUZZLE_DISTANCE).hit) {
                break;
            }
            const gc::Entity entity = m_replication.spawn(ARCHETYPE_BOLT, gc::NET_PEER_NONE, player.position + direction * BOLT_MUZZLE_DISTANCE);
            if (ProjectileComponent* const projectile = m_world.getComponent<ProjectileComponent>(entity); projectile) {
                projectile->velocity = direction * BOLT_SPEED;
                projectile->lifetime = BOLT_LIFETIME;
                projectile->shooter = shooter;
            }
            break;
        }
    }
}

// Authority only
void MatchSystem::respawn(const PlayerInfo& player)
{
    const SpawnPoint spawn_point = chooseSpawnPoint();
    if (player.owner == m_replication.getLocalPeerId()) {
        placeLocalPlayer(spawn_point.feet, spawn_point.yaw);
    }
    else {
        // The client decides where its player is, so it has to be asked to move it
        gc::NetEvent ev{};
        ev.type = MESSAGE_RESPAWN;
        ev.data.resize(RESPAWN_SIZE);
        gc::ByteWriter writer(ev.data);
        writeVec3(writer, spawn_point.feet);
        writer.writeF32(spawn_point.yaw);
        m_net.postEvent(ev, gc::NetDelivery::RELIABLE, player.owner);
    }
}

void MatchSystem::placeLocalPlayer(const glm::vec3& feet, float yaw)
{
    if (m_local_player == gc::ENTITY_NONE) {
        return;
    }
    gc::TransformComponent* const transform = m_world.getComponent<gc::TransformComponent>(m_local_player);
    transform->setPosition(feet + glm::vec3{0.0f, 0.0f, CHARACTER_EYE_HEIGHT});
    transform->setRotation(makeLookRotation(yaw, 0.0f));
    if (CharacterComponent* const character = m_world.getComponent<CharacterComponent>(m_local_player); character) {
        character->velocity = glm::vec3{0.0f};
        character->grounded = false;
        character->facing_known = false; // whatever moves the player takes the new direction from the transform
    }
}

void MatchSystem::addScore(gc::NetPeerId peer, int score)
{
    m_world.forEach<gc::ReplicatedComponent, PlayerComponent>([&](gc::Entity, const gc::ReplicatedComponent& replicated, PlayerComponent& player) {
        if (replicated.owner == peer) {
            player.score += score;
        }
    });
}

// Authority only. Tells whoever threw a bolt that it hit, so that their HUD can show it. It doesn't matter if this gets lost.
void MatchSystem::reportHit(gc::NetPeerId shooter)
{
    if (shooter == m_replication.getLocalPeerId()) {
        m_last_hit_time = m_time;
    }
    else if (m_net.getMode() == gc::NetMode::SERVER) {
        gc::NetEvent ev{};
        ev.type = MESSAGE_HIT;
        m_net.postEvent(ev, gc::NetDelivery::UNRELIABLE, shooter);
    }
}

// Authority only
void MatchSystem::reportKill(gc::NetPeerId killer, gc::NetPeerId victim)
{
    m_kill_feed.push_back(KillFeedEntry{killer, victim, m_time});
    if (m_net.getMode() == gc::NetMode::SERVER) {
        gc::NetEvent ev{};
        ev.type = MESSAGE_KILL;
        ev.data.resize(KILL_SIZE);
        gc::ByteWriter writer(ev.data);
        writer.writeU32(killer);
        writer.writeU32(victim);
        m_net.postEvent(ev, gc::NetDelivery::RELIABLE); // to every client
    }
}

std::vector<MatchSystem::PlayerInfo> MatchSystem::getPlayers()
{
    std::vector<PlayerInfo> players{};
    m_world.forEach<gc::TransformComponent, gc::ReplicatedComponent, PlayerComponent>(
        [&](gc::Entity entity, const gc::TransformComponent& t, const gc::ReplicatedComponent& replicated, const PlayerComponent&) {
            players.push_back(PlayerInfo{entity, replicated.owner, t.getPosition()});
        });
    return players;
}

void MatchSystem::simulateBolts(float delta_time)
{
    struct PlayerHit {
        PlayerInfo target;
        gc::NetPeerId shooter;
    };

    const std::vector<PlayerInfo> players = getPlayers();
    std::vector<glm::vec3> sentinels{};
    m_world.forEach<gc::TransformComponent, SentinelComponent>(
        [&](gc::Entity, const gc::TransformComponent& t, const SentinelComponent&) { sentinels.push_back(t.getPosition()); });

    // Entities aren't despawned or modified while iterating over them
    std::vector<gc::Entity> finished{};
    std::vector<PlayerHit> player_hits{};
    std::vector<gc::NetPeerId> sentinel_hits{};

    m_world.forEach<gc::TransformComponent, ProjectileComponent>([&](gc::Entity entity, gc::TransformComponent& t, ProjectileComponent& projectile) {
        const glm::vec3 from = t.getPosition();
        glm::vec3 to = from + projectile.velocity * delta_time;
        projectile.lifetime -= delta_time;
        bool done = (projectile.lifetime <= 0.0f);

        // Does the map stop it? Everything in the map that is solid has a ColliderComponent, which is all the CollisionSystem
        // needs. A ray along the whole path travelled this frame means that a fast bolt can't skip through a thin wall.
        const float step_length = glm::distance(from, to);
        if (step_length > 0.0f) {
            const gc::RaycastHit hit = m_collision.raycast(gc::Ray{from, to - from}, step_length);
            if (hit.hit) {
                to = hit.position; // nothing behind the wall can be hit
                done = true;
            }
        }

        bool hit_player = false;
        for (size_t i = 0; i < players.size() && !hit_player; ++i) {
            if (players[i].owner != projectile.shooter && distanceToSegment(getPlayerCentre(players[i].position), from, to) < PLAYER_HIT_RADIUS + BOLT_RADIUS) {
                player_hits.push_back(PlayerHit{players[i], projectile.shooter});
                hit_player = true;
                done = true;
            }
        }
        if (!hit_player) {
            for (const glm::vec3& sentinel : sentinels) {
                if (distanceToSegment(sentinel, from, to) < SENTINEL_RADIUS + BOLT_RADIUS) {
                    sentinel_hits.push_back(projectile.shooter);
                    done = true;
                    break;
                }
            }
        }

        if (done) {
            finished.push_back(entity);
        }
        else {
            t.setPosition(to);
        }
    });

    for (const gc::Entity entity : finished) {
        m_replication.despawn(entity);
    }
    for (const gc::NetPeerId shooter : sentinel_hits) {
        addScore(shooter, SCORE_SENTINEL);
        reportHit(shooter);
    }
    for (const PlayerHit& hit : player_hits) {
        PlayerComponent* const target = m_world.getComponent<PlayerComponent>(hit.target.entity);
        if (!target) {
            continue;
        }
        reportHit(hit.shooter);
        target->health -= BOLT_DAMAGE;
        if (target->health <= 0) {
            target->health = PLAYER_MAX_HEALTH;
            ++target->deaths;
            addScore(hit.shooter, SCORE_KILL);
            reportKill(hit.shooter, hit.target.owner);
            respawn(hit.target);
        }
    }
}

void MatchSystem::simulatePickups()
{
    const std::vector<PlayerInfo> players = getPlayers();
    for (uint32_t i = 0; i < m_pickup_spawners.size(); ++i) {
        PickupSpawner& spawner = m_pickup_spawners[i];
        if (spawner.pickup == gc::ENTITY_NONE) {
            if (m_time >= spawner.respawn_at) {
                spawnPickup(i);
            }
            continue;
        }
        for (const PlayerInfo& player : players) {
            if (glm::distance(getPlayerCentre(player.position), spawner.position) >= PICKUP_RADIUS) {
                continue;
            }
            if (spawner.kind == PickupKind::HEALTH) {
                PlayerComponent* const component = m_world.getComponent<PlayerComponent>(player.entity);
                if (!component || component->health >= PLAYER_MAX_HEALTH) {
                    continue; // left for someone who needs it
                }
                component->health = std::min(component->health + HEALTH_PICKUP_AMOUNT, PLAYER_MAX_HEALTH);
            }
            else {
                addScore(player.owner, SCORE_EMBER);
            }
            m_replication.despawn(spawner.pickup);
            spawner.pickup = gc::ENTITY_NONE;
            spawner.respawn_at = m_time + static_cast<double>(spawner.respawn_time);
            break;
        }
    }
}

void MatchSystem::simulateSentinels(float delta_time)
{
    m_world.forEach<gc::TransformComponent, SentinelComponent>([&](gc::Entity, gc::TransformComponent& t, SentinelComponent& sentinel) {
        if (sentinel.route >= m_patrol_routes.size()) {
            return;
        }
        const PatrolRoute& route = m_patrol_routes[sentinel.route];
        const glm::vec3 target = route.points[sentinel.next_point % route.points.size()].second;
        const glm::vec3 position = t.getPosition();
        const glm::vec3 to_target = target - position;
        const float distance = glm::length(to_target);
        const float step = SENTINEL_SPEED * delta_time;
        if (distance <= step) {
            t.setPosition(target);
            sentinel.next_point = (sentinel.next_point + 1) % static_cast<uint32_t>(route.points.size());
            return;
        }
        t.setPosition(position + to_target * (step / distance));
        // look where it is going
        if (glm::length(glm::vec2{to_target.x, to_target.y}) > 1.0e-3f) {
            t.setRotation(makeLookRotation(std::atan2(to_target.x, to_target.y), 0.0f));
        }
    });
}

void MatchSystem::updateLocalPlayer()
{
    const gc::NetPeerId local_peer = m_replication.getLocalPeerId();
    m_local_player = gc::ENTITY_NONE;
    m_world.forEach<gc::ReplicatedComponent, PlayerComponent>([&](gc::Entity entity, const gc::ReplicatedComponent& replicated, const PlayerComponent&) {
        if (replicated.owner == local_peer) {
            m_local_player = entity;
        }
    });

    if (m_overview_camera != gc::ENTITY_NONE) {
        // The player's camera is used if there is one. (CameraSystem uses the last active camera it finds)
        const bool has_player_camera = (m_local_player != gc::ENTITY_NONE && m_world.getComponent<gc::CameraComponent>(m_local_player));
        m_world.getComponent<gc::CameraComponent>(m_overview_camera)->setActive(!has_player_camera);
    }
}
