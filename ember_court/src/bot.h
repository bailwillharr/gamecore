#pragma once

// A computer controlled player, for filling a server without needing people (or a window).
// Bots walk to the nearest pickup and throw bolts at the nearest other player that they can see. They move with the same
// character controller as people do (character.h), and use the CollisionSystem's ray casts to tell what they can see.
// They don't plan a route: a bot that isn't getting anywhere tries another direction for a moment.
//
// In test mode a bot also checks that it sees what a client should see (other players moving, bolts, scores and health
// changing, smoothly moving server controlled entities) and then quits, so that replication can be tested automatically.

#include <random>
#include <unordered_map>

#include <glm/vec3.hpp>

#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>
#include <gamecore/gc_replication.h>

namespace gc {
class CollisionSystem; // forward-dec
}

class MatchSystem; // forward-dec

struct BotConfig {
    bool test{};
    float test_timeout{40.0f}; // seconds
    int* exit_code{};          // set when the test finishes
};

// Added to the local player when the game is run as a bot
struct BotComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("BotComponent");

    float fire_timer{};   // seconds until it can throw again
    float wander_angle{}; // which way it goes when there is nothing to go to

    // for noticing that it is walking into something, and getting round it
    float stuck_check_timer{};
    glm::vec3 stuck_check_position{};
    int stuck_count{};
    float avoid_timer{}; // while this is running it goes in avoid_direction instead
    glm::vec3 avoid_direction{};
    float jump_timer{};
};

class BotSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("BotSystem");

private:
    struct SentinelTrack {
        glm::vec3 last_position{};
        double first_seen_time{};
    };

    // What the bot has seen so far
    struct TestState {
        bool had_player{};
        bool saw_player_move{};
        bool saw_bolt{};
        bool saw_damage{};
        bool scored{};
        bool finished{};
        std::unordered_map<gc::NetEntityId, glm::vec3> player_first_positions{};
        std::unordered_map<gc::NetEntityId, SentinelTrack> sentinels{};
        uint64_t sentinel_frames{};
        uint64_t sentinel_jumps{};  // frames where a sentinel moved much further than it should have
        uint64_t sentinel_stalls{}; // frames where a sentinel didn't move at all
    };

    gc::ReplicationSystem& m_replication;
    gc::CollisionSystem& m_collision;
    MatchSystem& m_match;
    BotConfig m_config;
    std::mt19937 m_rng;
    double m_time{};
    TestState m_test{};

public:
    BotSystem(gc::World& world, gc::ReplicationSystem& replication, gc::CollisionSystem& collision, MatchSystem& match, const BotConfig& config);

    void onUpdate(gc::FrameState& frame_state) override;

private:
    void control(float delta_time);
    void updateTest(gc::FrameState& frame_state);
    void finishTest(bool passed, const char* reason);
};
