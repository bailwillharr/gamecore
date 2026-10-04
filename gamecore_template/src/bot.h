#pragma once

// A computer controlled player, for filling a server without needing people (or a window).
// Bots chase pickups and shoot at the nearest other player.
//
// In test mode a bot also checks that it sees what a client should see (other players moving, projectiles, scores and health
// changing, smoothly moving server controlled entities) and then quits, so that replication can be tested automatically.

#include <unordered_map>

#include <glm/vec3.hpp>

#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>
#include <gamecore/gc_replication.h>

class ArenaSystem; // forward-dec

struct BotConfig {
    bool test{};
    float test_timeout{40.0f}; // seconds
    int* exit_code{};          // set when the test finishes
};

// Added to the local player when the game is run as a bot
struct BotComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("BotComponent");

    float wander_angle{};
    float fire_timer{}; // seconds until it can fire again
};

class BotSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("BotSystem");

private:
    struct OrbiterTrack {
        glm::vec3 last_position{};
        double first_seen_time{};
    };

    // What the bot has seen so far
    struct TestState {
        bool had_player{};
        bool saw_player_move{};
        bool saw_projectile{};
        bool saw_damage{};
        bool scored{};
        bool finished{};
        std::unordered_map<gc::NetEntityId, glm::vec3> player_first_positions{};
        std::unordered_map<gc::NetEntityId, OrbiterTrack> orbiters{};
        uint64_t orbiter_frames{};
        uint64_t orbiter_jumps{};  // frames where an orbiter moved much further than it should have
        uint64_t orbiter_stalls{}; // frames where an orbiter didn't move at all
    };

    gc::ReplicationSystem& m_replication;
    ArenaSystem& m_arena;
    BotConfig m_config;
    double m_time{};
    TestState m_test{};

public:
    BotSystem(gc::World& world, gc::ReplicationSystem& replication, ArenaSystem& arena, const BotConfig& config);

    void onUpdate(gc::FrameState& frame_state) override;

private:
    void control(float delta_time);
    void updateTest(gc::FrameState& frame_state);
    void finishTest(bool passed, const char* reason);
};
