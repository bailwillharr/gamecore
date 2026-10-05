#pragma once

// The multiplayer test game.
//
// Players fly around an arena, collect pickups for points and shoot each other. Everything that matters is decided by the
// authority (the server, or the local game when offline):
//  - players:     owned by the client that controls them, which decides where they are (so movement is instant for the player).
//                 Their health and score (PlayerComponent) are decided by the server.
//  - projectiles: spawned by the server when a client asks to fire. Moved by the server, which also checks what they hit.
//  - pickups:     spawned by the server, which despawns them and awards a point when a player touches one.
//  - orbiters:    moved around the arena by the server. Shooting one is worth a point.
// The other hosts only show what the authority tells them.

#include <random>
#include <unordered_map>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <gamecore/gc_byte_reader.h>
#include <gamecore/gc_byte_writer.h>
#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>
#include <gamecore/gc_net_common.h>

namespace gc {
class Net;               // forward-dec
class ReplicationSystem; // forward-dec
} // namespace gc

inline constexpr gc::Name ARCHETYPE_PLAYER = gc::Name::createConstexpr("player");
inline constexpr gc::Name ARCHETYPE_PROJECTILE = gc::Name::createConstexpr("projectile");
inline constexpr gc::Name ARCHETYPE_PICKUP = gc::Name::createConstexpr("pickup");
inline constexpr gc::Name ARCHETYPE_ORBITER = gc::Name::createConstexpr("orbiter");

inline constexpr float PLAYER_EYE_HEIGHT = 67.5f * 25.4e-3f;
inline constexpr float PLAYER_MAX_HEALTH = 100.0f;
inline constexpr float ARENA_HALF_SIZE = 18.0f;      // pickups and spawn points are inside this
inline constexpr float ORBITER_MAX_SPEED = 8.0f;     // m/s
inline constexpr float PROJECTILE_SPEED = 30.0f;     // m/s
inline constexpr float PROJECTILE_LIFETIME = 2.0f;   // seconds
inline constexpr float FIRE_COOLDOWN = 0.25f;        // seconds
inline constexpr int PROJECTILE_DAMAGE = 34;
inline constexpr int NUM_PICKUPS = 8;
inline constexpr float CAMERA_EXPOSURE_EV100 = 7.0f; // suits the evening sun in game.cpp (400 lux)

struct GameConfig {
    bool headless{};           // nothing is rendered, so entities don't need models
    bool bot{};                // the local player is controlled by BotSystem
    bool spawn_local_player{}; // when this host is the authority, it has a player of its own
    bool exit_when_empty{};    // quit once every client that joined has left
    float freeze_time{};       // for testing. If not zero, nothing changes after this many seconds. See isFrozen()
};

// Replicated. Decided by the server.
struct PlayerComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("PlayerComponent");

    int32_t health{static_cast<int32_t>(PLAYER_MAX_HEALTH)};
    int32_t score{0};
    int32_t deaths{0};

    void serialise(gc::ByteWriter& writer) const
    {
        writer.writeU32(static_cast<uint32_t>(health));
        writer.writeU32(static_cast<uint32_t>(score));
        writer.writeU32(static_cast<uint32_t>(deaths));
    }

    void deserialise(gc::ByteReader& reader)
    {
        health = static_cast<int32_t>(reader.readU32());
        score = static_cast<int32_t>(reader.readU32());
        deaths = static_cast<int32_t>(reader.readU32());
    }
};

// The remaining components aren't replicated. The first three only exist on the authority, which runs the simulation.

struct ProjectileComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("ProjectileComponent");

    glm::vec3 velocity{};
    float lifetime{PROJECTILE_LIFETIME};
    gc::NetPeerId shooter{gc::NET_PEER_NONE};
};

struct PickupComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("PickupComponent");

    float radius{1.2f};
};

struct OrbiterComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("OrbiterComponent");

    glm::vec3 centre{};
    float radius{5.0f};
    float angular_speed{1.0f}; // radians per second
    float angle{0.0f};
};

// On players controlled by other hosts, when rendering. Keeps the model upright while the player looks up and down.
struct PlayerModelComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("PlayerModelComponent");

    gc::Entity model{gc::ENTITY_NONE};
};

class ArenaSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("ArenaSystem");

private:
    struct PlayerInfo {
        gc::Entity entity;
        gc::NetPeerId owner;
        glm::vec3 position;
    };

    gc::Net& m_net;
    gc::ReplicationSystem& m_replication;
    GameConfig m_config;

    std::mt19937 m_rng;
    double m_time{};
    double m_last_fire_request_time{-1.0};
    std::unordered_map<gc::NetPeerId, double> m_last_fire_times{}; // authority only, to stop clients firing faster than they should
    gc::Entity m_local_player{gc::ENTITY_NONE};
    gc::Entity m_spectator_camera{gc::ENTITY_NONE};
    uint32_t m_connected_peers{};
    bool m_digest_reported{false};

public:
    // Registers the game's replicated components and archetypes with the replication system
    ArenaSystem(gc::World& world, gc::Net& net, gc::ReplicationSystem& replication, const GameConfig& config);

    void onUpdate(gc::FrameState& frame_state) override;

    // ENTITY_NONE if the local host doesn't have a player (yet)
    gc::Entity getLocalPlayer() const;

    // For testing that every host ends up with the same state: once frozen, the simulation stops and players must stop too.
    // 5 seconds later a digest of all replicated state is logged, and clients exit.
    bool isFrozen() const;

    // Fires a projectile from the local player, if it has been long enough since the last one.
    // On a client this asks the server to do it. Returns false if nothing was fired.
    bool requestFire(const glm::vec3& direction);

private:
    void startWorld();
    void spawnPlayer(gc::NetPeerId owner);
    void spawnPickup();
    glm::vec3 randomPosition(float height);

    void fire(gc::NetPeerId shooter, glm::vec3 direction);
    void respawn(const PlayerInfo& player);
    void addScore(gc::NetPeerId peer, int score);
    std::vector<PlayerInfo> getPlayers();

    void simulateProjectiles(float delta_time);
    void simulatePickups();
    void simulateOrbiters(float delta_time);

    void updateLocalPlayer();
    void updatePlayerModels();
};
