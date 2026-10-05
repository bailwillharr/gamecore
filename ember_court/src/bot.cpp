#include "bot.h"

#include <cmath>

#include <algorithm>
#include <limits>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <gamecore/gc_app.h>
#include <gamecore/gc_collision_system.h>
#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_world.h>

#include "character.h"
#include "match.h"

static constexpr float BOT_FIRE_RANGE = 35.0f;       // m
static constexpr float BOT_FIRE_INTERVAL = 0.8f;     // seconds. Slower than a player can throw, so that bots aren't overwhelming
static constexpr float BOT_REACH_HEIGHT = 1.2f;      // m. Bots only go for pickups that are about level with them: they can't find ramps
static constexpr float STUCK_CHECK_INTERVAL = 0.5f;  // seconds
static constexpr double TEST_MIN_DURATION = 10.0;    // seconds. Long enough to judge how smooth interpolation is
static constexpr double TEST_SETTLE_TIME = 2.5;      // seconds an entity is ignored for after it first appears (the interpolation delay is still adapting)
static constexpr double TEST_MAX_ROUGH_RATIO = 0.08; // of frames where a server controlled entity wasn't moving smoothly

BotSystem::BotSystem(gc::World& world, gc::ReplicationSystem& replication, gc::CollisionSystem& collision, MatchSystem& match, const BotConfig& config)
    : gc::System(world), m_replication(replication), m_collision(collision), m_match(match), m_config(config), m_rng(std::random_device{}())
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
    const gc::Entity local_player = m_match.getLocalPlayer();
    if (local_player == gc::ENTITY_NONE || m_match.isFrozen()) {
        return;
    }
    BotComponent* const bot = m_world.getComponent<BotComponent>(local_player);
    CharacterComponent* const character = m_world.getComponent<CharacterComponent>(local_player);
    gc::TransformComponent* const transform = m_world.getComponent<gc::TransformComponent>(local_player);
    const PlayerComponent* const player = m_world.getComponent<PlayerComponent>(local_player);
    if (!bot || !character || !transform || !player) {
        return;
    }
    if (!character->facing_known) {
        getYawAndPitch(transform->getRotation(), character->yaw, character->pitch);
        character->facing_known = true;
    }

    const glm::vec3 eye_offset{0.0f, 0.0f, CHARACTER_EYE_HEIGHT};
    const glm::vec3 eye = transform->getPosition();
    glm::vec3 feet = eye - eye_offset;

    // find the nearest pickup that it can walk to and the nearest other player
    const bool wants_health = player->health <= PLAYER_MAX_HEALTH / 2;
    bool has_pickup = false;
    bool has_target = false;
    glm::vec3 pickup_position{};
    glm::vec3 target_position{};
    float pickup_distance = std::numeric_limits<float>::max();
    float target_distance = std::numeric_limits<float>::max();
    m_world.forEach<gc::TransformComponent, gc::ReplicatedComponent>(
        [&](gc::Entity entity, const gc::TransformComponent& t, const gc::ReplicatedComponent& replicated) {
            const float distance = glm::distance(t.getPosition(), eye);
            const bool is_pickup = (replicated.archetype == ARCHETYPE_EMBER) || (replicated.archetype == ARCHETYPE_HEALTH && wants_health);
            if (is_pickup && distance < pickup_distance && std::abs(t.getPosition().z - (feet.z + 1.0f)) < BOT_REACH_HEIGHT) {
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
    if (bot->avoid_timer > 0.0f) {
        bot->avoid_timer -= delta_time;
        move_direction = bot->avoid_direction;
    }
    else if (has_pickup) {
        move_direction = glm::vec3{pickup_position.x - feet.x, pickup_position.y - feet.y, 0.0f};
    }
    else {
        bot->wander_angle += 0.4f * delta_time;
        move_direction = glm::vec3{std::cos(bot->wander_angle), std::sin(bot->wander_angle), 0.0f};
    }
    if (const float length = glm::length(move_direction); length > 1.0e-3f) {
        move_direction /= length;
    }
    else {
        move_direction = glm::vec3{0.0f};
    }

    // Not getting anywhere? There is something in the way. Jump, and go off to one side for a while (longer every time, until it
    // works).
    bot->stuck_check_timer += delta_time;
    if (bot->stuck_check_timer >= STUCK_CHECK_INTERVAL) {
        const float moved = glm::distance(glm::vec2{feet.x, feet.y}, glm::vec2{bot->stuck_check_position.x, bot->stuck_check_position.y});
        if (moved < 0.3f * CHARACTER_SPEED * bot->stuck_check_timer) {
            bot->stuck_count = std::min(bot->stuck_count + 1, 6);
            const float turn =
                std::uniform_real_distribution<float>(0.3f, 0.75f)(m_rng) * glm::pi<float>() * (std::bernoulli_distribution(0.5)(m_rng) ? 1.0f : -1.0f);
            bot->avoid_direction = glm::angleAxis(turn, glm::vec3{0.0f, 0.0f, 1.0f}) * move_direction;
            bot->avoid_timer = 0.4f + 0.5f * static_cast<float>(bot->stuck_count);
            bot->jump_timer = 0.3f;
        }
        else if (bot->avoid_timer <= 0.0f) {
            bot->stuck_count = 0;
        }
        bot->stuck_check_timer = 0.0f;
        bot->stuck_check_position = feet;
    }

    CharacterInput input{};
    input.wish_direction = move_direction;
    input.jump = bot->jump_timer > 0.0f;
    bot->jump_timer -= delta_time;
    feet = moveCharacter(m_collision, *character, feet, input, delta_time);

    // Throw at the other player if nothing is in the way. The same ray cast that stops bolts tells the bot what it can see.
    bot->fire_timer -= delta_time;
    bool facing_target = false;
    if (has_target && target_distance < BOT_FIRE_RANGE && bot->fire_timer <= 0.0f) {
        const glm::vec3 new_eye = feet + eye_offset;
        const glm::vec3 target_centre = target_position - glm::vec3{0.0f, 0.0f, CHARACTER_EYE_HEIGHT - 0.5f * CHARACTER_HEIGHT};
        const glm::vec3 to_target = target_centre - new_eye;
        if (!m_collision.raycast(gc::Ray{new_eye, to_target}, glm::length(to_target)).hit) {
            if (m_match.requestFire(to_target)) {
                bot->fire_timer = BOT_FIRE_INTERVAL;
                getYawAndPitch(glm::quatLookAt(glm::normalize(to_target), glm::vec3{0.0f, 0.0f, 1.0f}), character->yaw, character->pitch);
                facing_target = true;
            }
        }
        else {
            bot->fire_timer = 0.2f; // look again soon, but not every frame
        }
    }
    if (!facing_target && bot->fire_timer < BOT_FIRE_INTERVAL - 0.3f && glm::length(move_direction) > 0.0f) {
        // look where it is going (after a moment of looking at what it threw at)
        character->yaw = std::atan2(move_direction.x, move_direction.y);
        character->pitch = 0.0f;
    }

    transform->setPosition(feet + eye_offset);
    transform->setRotation(makeLookRotation(character->yaw, character->pitch));
}

void BotSystem::updateTest(gc::FrameState& frame_state)
{
    for (const gc::NetEvent& ev : frame_state.net_events) {
        if (ev.kind == gc::NetEventKind::DISCONNECTED) {
            finishTest(false, gc::netDisconnectReasonString(ev.reason));
            return;
        }
    }

    const gc::Entity local_player = m_match.getLocalPlayer();
    if (local_player != gc::ENTITY_NONE) {
        m_test.had_player = true;
    }

    const float delta_time = static_cast<float>(frame_state.delta_time);
    m_world.forEach<gc::TransformComponent, gc::ReplicatedComponent>(
        [&](gc::Entity entity, const gc::TransformComponent& t, const gc::ReplicatedComponent& replicated) {
            if (replicated.archetype == ARCHETYPE_PLAYER) {
                const PlayerComponent* const player = m_world.getComponent<PlayerComponent>(entity);
                if (player && (player->health < PLAYER_MAX_HEALTH || player->deaths > 0)) {
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
            else if (replicated.archetype == ARCHETYPE_BOLT) {
                m_test.saw_bolt = true;
            }
            else if (replicated.archetype == ARCHETYPE_SENTINEL) {
                // Sentinels are moved by the server at a constant speed. If interpolation is working they move a little every
                // frame here too, however irregularly their states arrive.
                const auto [it, inserted] = m_test.sentinels.try_emplace(replicated.net_id, SentinelTrack{t.getPosition(), m_time});
                SentinelTrack& track = it->second;
                if (!inserted && m_time - track.first_seen_time > TEST_SETTLE_TIME) {
                    const float step = glm::distance(track.last_position, t.getPosition());
                    ++m_test.sentinel_frames;
                    if (step > SENTINEL_SPEED * delta_time * 3.0f + 0.02f) {
                        ++m_test.sentinel_jumps;
                    }
                    else if (step == 0.0f) {
                        ++m_test.sentinel_stalls;
                    }
                }
                track.last_position = t.getPosition();
            }
        });

    const bool seen_everything = m_test.had_player && m_test.saw_player_move && m_test.saw_bolt && m_test.saw_damage && m_test.scored;
    if (seen_everything && m_time >= TEST_MIN_DURATION && m_test.sentinel_frames > 0) {
        const double rough_ratio = static_cast<double>(m_test.sentinel_jumps + m_test.sentinel_stalls) / static_cast<double>(m_test.sentinel_frames);
        finishTest(rough_ratio <= TEST_MAX_ROUGH_RATIO, rough_ratio <= TEST_MAX_ROUGH_RATIO ? "saw everything" : "interpolation isn't smooth");
    }
    else if (m_time > static_cast<double>(m_config.test_timeout)) {
        finishTest(false, "timed out");
    }
}

void BotSystem::finishTest(bool passed, const char* reason)
{
    m_test.finished = true;

    const double frames = static_cast<double>(std::max<uint64_t>(m_test.sentinel_frames, 1));
    GC_INFO("BOT TEST {} ({}) after {:.1f} s: player spawned: {}, saw another player move: {}, saw a bolt: {}, saw damage: {}, scored: {}",
            passed ? "PASS" : "FAIL", reason, m_time, m_test.had_player, m_test.saw_player_move, m_test.saw_bolt, m_test.saw_damage, m_test.scored);
    GC_INFO("BOT TEST interpolation: {} frames, {:.2f}% jumps, {:.2f}% stalls", m_test.sentinel_frames,
            100.0 * static_cast<double>(m_test.sentinel_jumps) / frames, 100.0 * static_cast<double>(m_test.sentinel_stalls) / frames);
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
