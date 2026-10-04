#include "arena.h"

#include <cmath>

#include <algorithm>
#include <array>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <tracy/Tracy.hpp>

#include <gamecore/gc_app.h>
#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_net.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_replication.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_world.h>

#include "bot.h"
#include "mouse_move.h"
#include "spin.h"

static constexpr gc::Name MESSAGE_FIRE = gc::Name::createConstexpr("arena_fire");       // client -> server: direction (3 floats)
static constexpr gc::Name MESSAGE_RESPAWN = gc::Name::createConstexpr("arena_respawn"); // server -> client: position (3 floats)

static constexpr gc::Name MESH_PLAYER = gc::Name::createConstexpr("shrek.obj");
static constexpr gc::Name MESH_BALL = gc::Name::createConstexpr("ball");
static constexpr gc::Name MESH_CUBE = gc::Name::createConstexpr("cube");
static constexpr gc::Name MATERIAL_TEST = gc::Name::createConstexpr("testmat");
static constexpr gc::Name MATERIAL_BRICKS = gc::Name::createConstexpr("bricks-mortar");

static constexpr float PLAYER_HIT_RADIUS = 0.7f;
static constexpr float ORBITER_RADIUS = 1.0f;
static constexpr float PROJECTILE_RADIUS = 0.15f;
static constexpr size_t VEC3_SIZE = 3 * sizeof(float);

static float extractYaw(const glm::quat& rotation)
{
    // Camera local forward = -Z in its own space
    const glm::vec3 world_forward = rotation * glm::vec3(0.0f, 0.0f, -1.0f);
    // +X = right, -Y = forward (world), +Z = up, so yaw is the angle in the XY plane.
    return std::atan2(world_forward.x, -world_forward.y);
}

static bool isFinite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

static std::vector<uint8_t> packVec3(const glm::vec3& v)
{
    std::vector<uint8_t> data(VEC3_SIZE);
    gc::ByteWriter writer(data);
    writer.writeF32(v.x);
    writer.writeF32(v.y);
    writer.writeF32(v.z);
    return data;
}

static glm::vec3 unpackVec3(const std::vector<uint8_t>& data)
{
    gc::ByteReader reader(data);
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

static glm::vec3 getPlayerCentre(const glm::vec3& eye_position) { return eye_position - glm::vec3(0.0f, 0.0f, PLAYER_EYE_HEIGHT * 0.5f); }

// Replicated entities have no appearance of their own, they get a child entity to render (which isn't replicated)
static gc::Entity addModel(gc::World& world, gc::Entity parent, gc::Name mesh, gc::Name material, float scale)
{
    const gc::Entity model = world.createEntity(gc::Name::createConstexpr("model"), parent);
    world.getComponent<gc::TransformComponent>(model)->setScale(scale);
    world.addComponent<gc::RenderableComponent>(model).setMesh(mesh).setMaterial(material);
    return model;
}

ArenaSystem::ArenaSystem(gc::World& world, gc::Net& net, gc::ReplicationSystem& replication, const GameConfig& config)
    : gc::System(world), m_net(net), m_replication(replication), m_config(config), m_rng(std::random_device{}())
{
    m_replication.registerComponent<PlayerComponent>(gc::ReplicationAuthority::SERVER);

    {
        gc::ReplicationArchetype archetype{};
        archetype.transform_authority = gc::ReplicationAuthority::OWNER; // the owning client moves its own player
        archetype.on_spawn = [config](gc::World& world, gc::Entity entity, const gc::ReplicationSpawnInfo& info) {
            world.addComponent<PlayerComponent>(entity);
            if (info.locally_owned) {
                if (config.bot) {
                    world.addComponent<BotComponent>(entity);
                }
                else if (!config.headless) {
                    world.addComponent<gc::CameraComponent>(entity).setFOV(glm::radians(45.0f)).setNearPlane(0.1f).setActive(true);
                    world.addComponent<MouseMoveComponent>(entity).setMoveSpeed(10.0f).setAcceleration(100.0f).setDeceleration(100.0f).setSensitivity(3e-3f);
                }
            }
            else if (!config.headless) {
                const gc::Entity model = addModel(world, entity, MESH_PLAYER, gc::Name{}, 0.360f);
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
                addModel(world, entity, MESH_BALL, MATERIAL_BRICKS, PROJECTILE_RADIUS);
            }
        };
        m_replication.registerArchetype(ARCHETYPE_PROJECTILE, std::move(archetype));
    }
    {
        gc::ReplicationArchetype archetype{};
        archetype.transform_authority = gc::ReplicationAuthority::SERVER;
        archetype.interpolate_transform = false; // they don't move
        archetype.on_spawn = [config](gc::World& world, gc::Entity entity, const gc::ReplicationSpawnInfo& info) {
            if (info.has_authority) {
                world.addComponent<PickupComponent>(entity);
            }
            if (!config.headless) {
                // Spinning is only for show, so each host spins its own model instead of the server replicating the rotation
                const gc::Entity model = addModel(world, entity, MESH_CUBE, MATERIAL_TEST, 0.5f);
                world.addComponent<SpinComponent>(model).setAxis({0.0f, 0.0f, 1.0f}).setRadiansPerSecond(2.0f);
            }
        };
        m_replication.registerArchetype(ARCHETYPE_PICKUP, std::move(archetype));
    }
    {
        gc::ReplicationArchetype archetype{};
        archetype.transform_authority = gc::ReplicationAuthority::SERVER;
        archetype.on_spawn = [config](gc::World& world, gc::Entity entity, const gc::ReplicationSpawnInfo& info) {
            if (info.has_authority) {
                world.addComponent<OrbiterComponent>(entity);
            }
            if (!config.headless) {
                addModel(world, entity, MESH_BALL, MATERIAL_TEST, ORBITER_RADIUS);
            }
        };
        m_replication.registerArchetype(ARCHETYPE_ORBITER, std::move(archetype));
    }

    if (!m_config.headless) {
        // Used whenever the local host doesn't have a player, e.g. while connecting
        const glm::vec3 position{0.0f, -30.0f, 15.0f};
        m_spectator_camera = m_world.createEntity(gc::Name::createConstexpr("spectator_camera"), gc::ENTITY_NONE, position,
                                                  glm::quatLookAt(glm::normalize(-position), glm::vec3{0.0f, 0.0f, 1.0f}));
        m_world.addComponent<gc::CameraComponent>(m_spectator_camera).setFOV(glm::radians(45.0f)).setNearPlane(0.1f).setActive(true);
    }
}

void ArenaSystem::onUpdate(gc::FrameState& frame_state)
{
    ZoneScoped;

    m_time += frame_state.delta_time;

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

    for (const gc::NetEvent& ev : frame_state.net_events) {
        if (ev.kind != gc::NetEventKind::MESSAGE || ev.data.size() != VEC3_SIZE) {
            continue;
        }
        if (ev.type == MESSAGE_FIRE && m_replication.hasAuthority()) {
            fire(ev.peer, unpackVec3(ev.data));
        }
        else if (ev.type == MESSAGE_RESPAWN && !m_replication.hasAuthority() && ev.peer == gc::NET_PEER_SERVER) {
            const glm::vec3 position = unpackVec3(ev.data);
            if (m_local_player != gc::ENTITY_NONE && isFinite(position)) {
                m_world.getComponent<gc::TransformComponent>(m_local_player)->setPosition(position);
            }
        }
    }

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

    if (frame_state.window_state && m_local_player != gc::ENTITY_NONE && !m_config.bot) {
        if (frame_state.window_state->getIsMouseCaptured() && frame_state.window_state->getButtonDown(gc::MouseButton::LEFT)) {
            const glm::quat rotation = m_world.getComponent<gc::TransformComponent>(m_local_player)->getRotation();
            requestFire(rotation * glm::vec3{0.0f, 0.0f, -1.0f});
        }
    }

    if (m_replication.hasAuthority()) {
        const float delta_time = static_cast<float>(frame_state.delta_time);
        simulateProjectiles(delta_time);
        simulatePickups();
        simulateOrbiters(delta_time);
    }

    if (!m_config.headless) {
        updatePlayerModels();
    }
}

gc::Entity ArenaSystem::getLocalPlayer() const { return m_local_player; }

bool ArenaSystem::isFrozen() const { return m_config.freeze_time > 0.0f && m_time >= static_cast<double>(m_config.freeze_time); }

bool ArenaSystem::requestFire(const glm::vec3& direction)
{
    if (m_local_player == gc::ENTITY_NONE || isFrozen() || m_time - m_last_fire_request_time < static_cast<double>(FIRE_COOLDOWN)) {
        return false;
    }
    m_last_fire_request_time = m_time;

    if (m_replication.hasAuthority()) {
        fire(m_replication.getLocalPeerId(), direction);
    }
    else {
        // Only the server can spawn things. It doesn't matter much if the projectile appears a moment late, but it must appear.
        gc::NetEvent ev{};
        ev.type = gc::Name("arena_fire");
        ev.data = packVec3(direction);
        m_net.postEvent(ev, gc::NetDelivery::RELIABLE);
    }
    return true;
}

void ArenaSystem::startWorld()
{
    GC_INFO("Spawning the arena");

    for (int i = 0; i < NUM_PICKUPS; ++i) {
        spawnPickup();
    }

    struct OrbiterSetup {
        float radius;
        float height;
        float angular_speed;
    };
    // radius * angular speed must stay below ORBITER_MAX_SPEED
    constexpr std::array<OrbiterSetup, 3> ORBITERS{{{6.0f, 1.5f, 0.9f}, {10.0f, 2.5f, -0.6f}, {14.0f, 3.5f, 0.5f}}};
    for (const OrbiterSetup& setup : ORBITERS) {
        const gc::Entity entity = m_replication.spawn(ARCHETYPE_ORBITER, gc::NET_PEER_NONE, {setup.radius, 0.0f, setup.height});
        OrbiterComponent* const orbiter = m_world.getComponent<OrbiterComponent>(entity);
        orbiter->centre = {0.0f, 0.0f, setup.height};
        orbiter->radius = setup.radius;
        orbiter->angular_speed = setup.angular_speed;
    }

    if (m_config.spawn_local_player) {
        spawnPlayer(m_replication.getLocalPeerId());
    }
}

void ArenaSystem::spawnPlayer(gc::NetPeerId owner) { m_replication.spawn(ARCHETYPE_PLAYER, owner, randomPosition(PLAYER_EYE_HEIGHT)); }

void ArenaSystem::spawnPickup() { m_replication.spawn(ARCHETYPE_PICKUP, gc::NET_PEER_NONE, randomPosition(1.0f)); }

glm::vec3 ArenaSystem::randomPosition(float height)
{
    std::uniform_real_distribution<float> distribution(-ARENA_HALF_SIZE, ARENA_HALF_SIZE);
    const float x = distribution(m_rng);
    const float y = distribution(m_rng);
    return {x, y, height};
}

// Authority only
void ArenaSystem::fire(gc::NetPeerId shooter, glm::vec3 direction)
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
            // start outside the shooter so that the projectile is visible from first person
            const glm::vec3 origin = player.position + direction * 1.0f;
            const gc::Entity entity = m_replication.spawn(ARCHETYPE_PROJECTILE, gc::NET_PEER_NONE, origin);
            ProjectileComponent* const projectile = m_world.getComponent<ProjectileComponent>(entity);
            projectile->velocity = direction * PROJECTILE_SPEED;
            projectile->shooter = shooter;
            break;
        }
    }
}

// Authority only
void ArenaSystem::respawn(const PlayerInfo& player)
{
    const glm::vec3 position = randomPosition(PLAYER_EYE_HEIGHT);
    if (player.owner == m_replication.getLocalPeerId()) {
        m_world.getComponent<gc::TransformComponent>(player.entity)->setPosition(position);
    }
    else {
        // The client decides where its player is, so it has to be asked to move it
        gc::NetEvent ev{};
        ev.type = gc::Name("arena_respawn");
        ev.data = packVec3(position);
        m_net.postEvent(ev, gc::NetDelivery::RELIABLE, player.owner);
    }
}

void ArenaSystem::addScore(gc::NetPeerId peer, int score)
{
    m_world.forEach<gc::ReplicatedComponent, PlayerComponent>([&](gc::Entity, const gc::ReplicatedComponent& replicated, PlayerComponent& player) {
        if (replicated.owner == peer) {
            player.score += score;
        }
    });
}

std::vector<ArenaSystem::PlayerInfo> ArenaSystem::getPlayers()
{
    std::vector<PlayerInfo> players{};
    m_world.forEach<gc::TransformComponent, gc::ReplicatedComponent, PlayerComponent>(
        [&](gc::Entity entity, const gc::TransformComponent& t, const gc::ReplicatedComponent& replicated, const PlayerComponent&) {
            players.push_back(PlayerInfo{entity, replicated.owner, t.getPosition()});
        });
    return players;
}

void ArenaSystem::simulateProjectiles(float delta_time)
{
    struct PlayerHit {
        PlayerInfo target;
        gc::NetPeerId shooter;
    };

    const std::vector<PlayerInfo> players = getPlayers();
    std::vector<glm::vec3> orbiters{};
    m_world.forEach<gc::TransformComponent, OrbiterComponent>(
        [&](gc::Entity, const gc::TransformComponent& t, const OrbiterComponent&) { orbiters.push_back(t.getPosition()); });

    // Entities aren't despawned or modified while iterating over them
    std::vector<gc::Entity> finished{};
    std::vector<PlayerHit> player_hits{};
    std::vector<gc::NetPeerId> orbiter_hits{};

    m_world.forEach<gc::TransformComponent, ProjectileComponent>([&](gc::Entity entity, gc::TransformComponent& t, ProjectileComponent& projectile) {
        const glm::vec3 from = t.getPosition();
        const glm::vec3 to = from + projectile.velocity * delta_time;
        projectile.lifetime -= delta_time;

        bool done = (projectile.lifetime <= 0.0f || to.z < 0.0f);
        // Checking the whole path travelled this frame means fast projectiles can't skip through things
        for (size_t i = 0; i < players.size() && !done; ++i) {
            if (players[i].owner != projectile.shooter &&
                distanceToSegment(getPlayerCentre(players[i].position), from, to) < PLAYER_HIT_RADIUS + PROJECTILE_RADIUS) {
                player_hits.push_back(PlayerHit{players[i], projectile.shooter});
                done = true;
            }
        }
        for (size_t i = 0; i < orbiters.size() && !done; ++i) {
            if (distanceToSegment(orbiters[i], from, to) < ORBITER_RADIUS + PROJECTILE_RADIUS) {
                orbiter_hits.push_back(projectile.shooter);
                done = true;
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
    for (const gc::NetPeerId shooter : orbiter_hits) {
        addScore(shooter, 1);
    }
    for (const PlayerHit& hit : player_hits) {
        PlayerComponent* const target = m_world.getComponent<PlayerComponent>(hit.target.entity);
        if (!target) {
            continue;
        }
        target->health -= PROJECTILE_DAMAGE;
        if (target->health <= 0) {
            target->health = static_cast<int32_t>(PLAYER_MAX_HEALTH);
            ++target->deaths;
            addScore(hit.shooter, 3);
            respawn(hit.target);
        }
    }
}

void ArenaSystem::simulatePickups()
{
    struct Collected {
        gc::Entity pickup;
        gc::NetPeerId collector;
    };

    const std::vector<PlayerInfo> players = getPlayers();
    std::vector<Collected> collected{};
    m_world.forEach<gc::TransformComponent, PickupComponent>([&](gc::Entity entity, const gc::TransformComponent& t, const PickupComponent& pickup) {
        for (const PlayerInfo& player : players) {
            if (glm::distance(getPlayerCentre(player.position), t.getPosition()) < pickup.radius) {
                collected.push_back(Collected{entity, player.owner});
                break;
            }
        }
    });

    for (const Collected& c : collected) {
        addScore(c.collector, 1);
        m_replication.despawn(c.pickup);
        spawnPickup();
    }
}

void ArenaSystem::simulateOrbiters(float delta_time)
{
    m_world.forEach<gc::TransformComponent, OrbiterComponent>([&](gc::Entity, gc::TransformComponent& t, OrbiterComponent& orbiter) {
        orbiter.angle = std::fmod(orbiter.angle + orbiter.angular_speed * delta_time, glm::two_pi<float>());
        t.setPosition(orbiter.centre + glm::vec3{std::cos(orbiter.angle), std::sin(orbiter.angle), 0.0f} * orbiter.radius);
    });
}

void ArenaSystem::updateLocalPlayer()
{
    const gc::NetPeerId local_peer = m_replication.getLocalPeerId();
    m_local_player = gc::ENTITY_NONE;
    m_world.forEach<gc::ReplicatedComponent, PlayerComponent>([&](gc::Entity entity, const gc::ReplicatedComponent& replicated, const PlayerComponent&) {
        if (replicated.owner == local_peer) {
            m_local_player = entity;
        }
    });

    if (m_spectator_camera != gc::ENTITY_NONE) {
        // The player's camera is used if there is one. (CameraSystem uses the last active camera it finds)
        const bool has_player_camera = (m_local_player != gc::ENTITY_NONE && m_world.getComponent<gc::CameraComponent>(m_local_player));
        m_world.getComponent<gc::CameraComponent>(m_spectator_camera)->setActive(!has_player_camera);
    }
}

// A player's transform is its camera, which pitches up and down. Its model should only turn left and right.
void ArenaSystem::updatePlayerModels()
{
    struct Update {
        gc::Entity model;
        glm::vec3 position;
        glm::quat rotation;
    };
    std::vector<Update> updates{};
    m_world.forEach<gc::TransformComponent, PlayerModelComponent>([&](gc::Entity, const gc::TransformComponent& t, const PlayerModelComponent& player_model) {
        const glm::quat inverse_rotation = glm::inverse(t.getRotation());
        const glm::quat yaw = glm::angleAxis(extractYaw(t.getRotation()), glm::vec3{0.0f, 0.0f, 1.0f});
        // cancel out the parent's rotation, then apply only the yaw. The model's origin is at its feet.
        updates.push_back(Update{player_model.model, inverse_rotation * glm::vec3{0.0f, 0.0f, -PLAYER_EYE_HEIGHT}, inverse_rotation * yaw});
    });
    for (const Update& update : updates) {
        gc::TransformComponent* const t = m_world.getComponent<gc::TransformComponent>(update.model);
        if (t && (t->getPosition() != update.position || t->getRotation() != update.rotation)) {
            t->setPosition(update.position);
            t->setRotation(update.rotation);
        }
    }
}
