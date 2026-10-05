#pragma once

// The rules of Ember Court.
//
// Players run around a walled court, collect embers for points and throw fire bolts at each other. Everything that matters is
// decided by the authority (the server, or the local game when offline):
//  - players:    owned by the client that controls them, which decides where they are (so movement is instant for the player).
//                Their health and score (PlayerComponent) are decided by the server.
//  - fire bolts: spawned by the server when a client asks to fire. Moved by the server, which also checks what they hit: the
//                map (with the CollisionSystem), players and sentinels.
//  - pickups:    spawned by the server at the map's PickupSpawnerComponents, and again a while after they are taken.
//  - sentinels:  flown round the map's patrol routes by the server. Hitting one is worth points.
// The other hosts only show what the authority tells them.
//
// The map itself (content/ember_court.gcpak) is one prefab that every host loads, so none of it is replicated. Where players and
// pickups appear, and where sentinels fly, is read from components in it: see components.h.

#include <random>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>

#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>
#include <gamecore/gc_net_common.h>

#include "components.h"

namespace gc {
class CollisionSystem;   // forward-dec
class Net;               // forward-dec
class ReplicationSystem; // forward-dec
class ResourceManager;   // forward-dec
} // namespace gc

inline constexpr gc::Name ARCHETYPE_PLAYER = gc::Name::createConstexpr("player");
inline constexpr gc::Name ARCHETYPE_BOLT = gc::Name::createConstexpr("bolt");
inline constexpr gc::Name ARCHETYPE_EMBER = gc::Name::createConstexpr("ember");
inline constexpr gc::Name ARCHETYPE_HEALTH = gc::Name::createConstexpr("health");
inline constexpr gc::Name ARCHETYPE_SENTINEL = gc::Name::createConstexpr("sentinel");

inline constexpr gc::Name MAP_PREFAB = gc::Name::createConstexpr(MAP_PREFAB_NAME);

inline constexpr float SENTINEL_SPEED = 4.0f; // m/s
inline constexpr float BOLT_SPEED = 40.0f;    // m/s
inline constexpr float BOLT_LIFETIME = 2.5f;  // seconds
inline constexpr float FIRE_COOLDOWN = 0.3f;  // seconds
inline constexpr int BOLT_DAMAGE = 34;
inline constexpr int HEALTH_PICKUP_AMOUNT = 50;
inline constexpr int SCORE_EMBER = 1;
inline constexpr int SCORE_SENTINEL = 2;
inline constexpr int SCORE_KILL = 3;
inline constexpr float CAMERA_EXPOSURE_EV100 = 7.5f; // suits the map's evening sun. The map's own camera has the same

struct GameConfig {
    bool headless{};           // nothing is rendered, so entities don't need models
    bool bot{};                // the local player is controlled by BotSystem
    bool spawn_local_player{}; // when this host is the authority, it has a player of its own
    bool exit_when_empty{};    // quit once every client that joined has left
    float freeze_time{};       // for testing. If not zero, nothing changes after this many seconds. See isFrozen()
};

// Makes the meshes and materials of the things that aren't part of the map (players, bolts, pickups, sentinels).
// Only for hosts that render.
void createGameResources(gc::ResourceManager& resource_manager);

class MatchSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("MatchSystem");

    struct KillFeedEntry {
        gc::NetPeerId killer{};
        gc::NetPeerId victim{};
        double time{}; // see getTime()
    };

private:
    struct PlayerInfo {
        gc::Entity entity;
        gc::NetPeerId owner;
        glm::vec3 position; // of its eyes
    };

    // What the map says, read from its prefab's components
    struct SpawnPoint {
        glm::vec3 feet{};
        float yaw{};
    };
    struct PickupSpawner {
        glm::vec3 position{};
        PickupKind kind{};
        float respawn_time{};
        gc::Entity pickup{gc::ENTITY_NONE}; // authority only: the pickup that is there now
        double respawn_at{};                // authority only: when the next one appears
    };
    struct PatrolRoute {
        uint32_t id{};
        std::vector<std::pair<uint32_t, glm::vec3>> points{}; // order and position, sorted by order
    };

    gc::Net& m_net;
    gc::ReplicationSystem& m_replication;
    gc::CollisionSystem& m_collision;
    GameConfig m_config;

    std::mt19937 m_rng;
    double m_time{};
    bool m_map_read{false};
    std::vector<SpawnPoint> m_spawn_points{};
    std::vector<PickupSpawner> m_pickup_spawners{};
    std::vector<PatrolRoute> m_patrol_routes{};

    double m_last_fire_request_time{-1.0};
    std::unordered_map<gc::NetPeerId, double> m_last_fire_times{}; // authority only, to stop clients firing faster than they should
    gc::Entity m_local_player{gc::ENTITY_NONE};
    gc::Entity m_overview_camera{gc::ENTITY_NONE};
    uint32_t m_connected_peers{};
    bool m_digest_reported{false};

    std::vector<KillFeedEntry> m_kill_feed{};
    double m_last_hit_time{-1.0};

public:
    // Registers the game's replicated components and archetypes with the replication system
    MatchSystem(gc::World& world, gc::Net& net, gc::ReplicationSystem& replication, gc::CollisionSystem& collision, const GameConfig& config);

    void onUpdate(gc::FrameState& frame_state) override;

    // ENTITY_NONE if the local host doesn't have a player (yet)
    gc::Entity getLocalPlayer() const;

    // For testing that every host ends up with the same state: once frozen, the simulation stops and players must stop too.
    // 5 seconds later a digest of all replicated state is logged, and clients exit.
    bool isFrozen() const;

    // Throws a fire bolt from the local player, if it has been long enough since the last one.
    // On a client this asks the server to do it. Returns false if nothing was thrown.
    bool requestFire(const glm::vec3& direction);

    // Seconds since the game started on this host
    double getTime() const;

    // For the HUD: who burned who, most recent last, and when a bolt thrown by the local player last hit something
    const std::vector<KillFeedEntry>& getKillFeed() const;
    double getLastHitTime() const;

private:
    void readMap();
    void startWorld();
    void spawnPlayer(gc::NetPeerId owner);
    void spawnPickup(uint32_t spawner_index);
    SpawnPoint chooseSpawnPoint();

    void handleNetEvents(const gc::FrameState& frame_state);
    void fire(gc::NetPeerId shooter, glm::vec3 direction);
    void respawn(const PlayerInfo& player);
    void placeLocalPlayer(const glm::vec3& feet, float yaw);
    void addScore(gc::NetPeerId peer, int score);
    void reportHit(gc::NetPeerId shooter);
    void reportKill(gc::NetPeerId killer, gc::NetPeerId victim);
    std::vector<PlayerInfo> getPlayers();

    void simulateBolts(float delta_time);
    void simulatePickups();
    void simulateSentinels(float delta_time);

    void updateLocalPlayer();
};
