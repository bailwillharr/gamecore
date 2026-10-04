#include "bot.h"

#include <cmath>

#include <algorithm>
#include <limits>
#include <string>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <gamecore/gc_app.h>
#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_world.h>

#include "arena.h"

static constexpr float BOT_SPEED = 7.0f;             // m/s
static constexpr float BOT_FIRE_RANGE = 40.0f;       // m
static constexpr float BOT_FIRE_INTERVAL = 0.8f;     // seconds. Slower than a player can fire, so that bots aren't overwhelming
static constexpr double TEST_MIN_DURATION = 10.0;    // seconds. Long enough to judge how smooth interpolation is
static constexpr double TEST_SETTLE_TIME = 2.5;      // seconds an entity is ignored for after it first appears (the interpolation delay is still adapting)
static constexpr double TEST_MAX_ROUGH_RATIO = 0.08; // of frames where a server controlled entity wasn't moving smoothly

BotSystem::BotSystem(gc::World& world, gc::ReplicationSystem& replication, ArenaSystem& arena, const BotConfig& config)
    : gc::System(world), m_replication(replication), m_arena(arena), m_config(config)
{
}

void BotSystem::onUpdate(gc::FrameState& frame_state)
{
    m_time += frame_state.delta_time;
    control(static_cast<float>(frame_state.delta_time));
    if (m_config.test && !m_test.finished) {
        updateTest(frame_state);
    }
}

void BotSystem::control(float delta_time)
{
    const gc::Entity local_player = m_arena.getLocalPlayer();
    if (local_player == gc::ENTITY_NONE || m_arena.isFrozen()) {
        return;
    }
    BotComponent* const bot = m_world.getComponent<BotComponent>(local_player);
    gc::TransformComponent* const transform = m_world.getComponent<gc::TransformComponent>(local_player);
    if (!bot || !transform) {
        return;
    }
    glm::vec3 position = transform->getPosition();

    // find the nearest pickup and the nearest other player
    bool has_pickup = false;
    bool has_target = false;
    glm::vec3 pickup_position{};
    glm::vec3 target_position{};
    float pickup_distance = std::numeric_limits<float>::max();
    float target_distance = std::numeric_limits<float>::max();
    m_world.forEach<gc::TransformComponent, gc::ReplicatedComponent>(
        [&](gc::Entity entity, const gc::TransformComponent& t, const gc::ReplicatedComponent& replicated) {
            const float distance = glm::distance(t.getPosition(), position);
            if (replicated.archetype == ARCHETYPE_PICKUP && distance < pickup_distance) {
                has_pickup = true;
                pickup_position = t.getPosition();
                pickup_distance = distance;
            }
            else if (replicated.archetype == ARCHETYPE_PLAYER && entity != local_player && distance < target_distance) {
                has_target = true;
                target_position = t.getPosition();
                target_distance = distance;
            }
        });

    // head for the pickup, or wander in circles if there isn't one
    glm::vec3 move_direction{};
    if (has_pickup) {
        move_direction = glm::vec3{pickup_position.x - position.x, pickup_position.y - position.y, 0.0f};
    }
    else {
        bot->wander_angle += delta_time;
        move_direction = glm::vec3{std::cos(bot->wander_angle), std::sin(bot->wander_angle), 0.0f};
    }
    if (glm::length(move_direction) > 1.0e-3f) {
        move_direction = glm::normalize(move_direction);
        position += move_direction * BOT_SPEED * delta_time;
        position.z = PLAYER_EYE_HEIGHT;

        // the same convention as MouseMoveSystem: a player's transform is a camera that looks along its local -Z
        const float yaw = std::atan2(move_direction.x, move_direction.y);
        const glm::quat rotation = glm::angleAxis(-yaw, glm::vec3{0.0f, 0.0f, 1.0f}) * glm::angleAxis(glm::half_pi<float>(), glm::vec3{1.0f, 0.0f, 0.0f});
        transform->setPosition(position);
        transform->setRotation(rotation);
    }

    bot->fire_timer -= delta_time;
    if (has_target && target_distance < BOT_FIRE_RANGE && bot->fire_timer <= 0.0f) {
        const glm::vec3 target_centre = target_position - glm::vec3{0.0f, 0.0f, PLAYER_EYE_HEIGHT * 0.5f};
        if (m_arena.requestFire(target_centre - position)) {
            bot->fire_timer = BOT_FIRE_INTERVAL;
        }
    }
}

void BotSystem::updateTest(gc::FrameState& frame_state)
{
    for (const gc::NetEvent& ev : frame_state.net_events) {
        if (ev.kind == gc::NetEventKind::DISCONNECTED) {
            finishTest(false, gc::netDisconnectReasonString(ev.reason));
            return;
        }
    }

    const gc::Entity local_player = m_arena.getLocalPlayer();
    if (local_player != gc::ENTITY_NONE) {
        m_test.had_player = true;
    }

    const float delta_time = static_cast<float>(frame_state.delta_time);
    m_world.forEach<gc::TransformComponent, gc::ReplicatedComponent>(
        [&](gc::Entity entity, const gc::TransformComponent& t, const gc::ReplicatedComponent& replicated) {
            if (replicated.archetype == ARCHETYPE_PLAYER) {
                const PlayerComponent* const player = m_world.getComponent<PlayerComponent>(entity);
                if (player && (player->health < static_cast<int32_t>(PLAYER_MAX_HEALTH) || player->deaths > 0)) {
                    m_test.saw_damage = true;
                }
                if (player && entity == local_player && player->score > 0) {
                    m_test.scored = true;
                }
                if (entity != local_player) {
                    const auto [it, inserted] = m_test.player_first_positions.try_emplace(replicated.net_id, t.getPosition());
                    if (glm::distance(it->second, t.getPosition()) > 2.0f) {
                        m_test.saw_player_move = true;
                    }
                }
            }
            else if (replicated.archetype == ARCHETYPE_PROJECTILE) {
                m_test.saw_projectile = true;
            }
            else if (replicated.archetype == ARCHETYPE_ORBITER) {
                // Orbiters are moved by the server at a constant speed. If interpolation is working they move a little every
                // frame here too, however irregularly their states arrive.
                const auto [it, inserted] = m_test.orbiters.try_emplace(replicated.net_id, OrbiterTrack{t.getPosition(), m_time});
                OrbiterTrack& track = it->second;
                if (!inserted && m_time - track.first_seen_time > TEST_SETTLE_TIME) {
                    const float step = glm::distance(track.last_position, t.getPosition());
                    ++m_test.orbiter_frames;
                    if (step > ORBITER_MAX_SPEED * delta_time * 3.0f + 0.02f) {
                        ++m_test.orbiter_jumps;
                    }
                    else if (step == 0.0f) {
                        ++m_test.orbiter_stalls;
                    }
                }
                track.last_position = t.getPosition();
            }
        });

    const bool seen_everything = m_test.had_player && m_test.saw_player_move && m_test.saw_projectile && m_test.saw_damage && m_test.scored;
    if (seen_everything && m_time >= TEST_MIN_DURATION && m_test.orbiter_frames > 0) {
        const double rough_ratio = static_cast<double>(m_test.orbiter_jumps + m_test.orbiter_stalls) / static_cast<double>(m_test.orbiter_frames);
        finishTest(rough_ratio <= TEST_MAX_ROUGH_RATIO, rough_ratio <= TEST_MAX_ROUGH_RATIO ? "saw everything" : "interpolation isn't smooth");
    }
    else if (m_time > static_cast<double>(m_config.test_timeout)) {
        finishTest(false, "timed out");
    }
}

void BotSystem::finishTest(bool passed, const char* reason)
{
    m_test.finished = true;

    const double frames = static_cast<double>(std::max<uint64_t>(m_test.orbiter_frames, 1));
    GC_INFO("BOT TEST {} ({}) after {:.1f} s: player spawned: {}, saw another player move: {}, saw a projectile: {}, saw damage: {}, scored: {}",
            passed ? "PASS" : "FAIL", reason, m_time, m_test.had_player, m_test.saw_player_move, m_test.saw_projectile, m_test.saw_damage, m_test.scored);
    GC_INFO("BOT TEST interpolation: {} frames, {:.2f}% jumps, {:.2f}% stalls", m_test.orbiter_frames,
            100.0 * static_cast<double>(m_test.orbiter_jumps) / frames, 100.0 * static_cast<double>(m_test.orbiter_stalls) / frames);
    for (const gc::ReplicationLinkStats& stats : m_replication.getLinkStats()) {
        GC_INFO("BOT TEST state messages: jitter {:.1f} ms, loss {:.1f}%, interpolation delay {:.0f} ms", stats.jitter * 1.0e3f, stats.loss * 100.0f,
                stats.interpolation_delay * 1.0e3f);
        GC_INFO("BOT TEST replication: {} entities, state messages out/in {}/{}, states out/resent/in {}/{}/{}, bytes out/in {}/{}", stats.entities,
                stats.messages_sent, stats.messages_received, stats.states_sent, stats.states_resent, stats.states_received, stats.bytes_sent,
                stats.bytes_received);
    }

    if (m_config.exit_code) {
        *m_config.exit_code = passed ? 0 : 1;
    }
    gc::App::instance().requestQuit();
}
