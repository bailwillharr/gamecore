#pragma once

// The components that Ember Court adds to the engine's.
//
// The first group is stored in the map's prefab: they are SerialisableComponents (see gc_ecs.h), written by the map generator
// (mapgen/) and found by name when the game loads the prefab. They are how the map tells the game where things go.
// The second group is replicated. The rest only exist on the hosts that need them.

#include <cstdint>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <gamecore/gc_byte_reader.h>
#include <gamecore/gc_byte_writer.h>
#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>
#include <gamecore/gc_net_common.h>

// The map: a prefab asset in content/ember_court.gcpak
inline constexpr const char* MAP_PREFAB_NAME = "ember_court.map";

//
// In the map prefab
//

// Somewhere a player can appear. The entity's position is where their feet go
struct SpawnPointComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("SpawnPointComponent");

    float yaw{}; // radians. The way the player faces: zero is +Y, positive turns towards +X

    void serialise(gc::ByteWriter& writer) const { writer.writeF32(yaw); }
    void deserialise(gc::ByteReader& reader) { yaw = reader.readF32(); }
    static constexpr size_t getSerialisedSize() { return sizeof(float); }
};

enum class PickupKind : uint8_t {
    EMBER = 0,  // worth a point
    HEALTH = 1, // heals
};

// Somewhere that a pickup appears, and appears again a while after it is taken
struct PickupSpawnerComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("PickupSpawnerComponent");

    PickupKind kind{PickupKind::EMBER};
    float respawn_time{8.0f}; // seconds

    void serialise(gc::ByteWriter& writer) const
    {
        writer.writeU8(static_cast<uint8_t>(kind));
        writer.writeF32(respawn_time);
    }
    void deserialise(gc::ByteReader& reader)
    {
        kind = (reader.readU8() == static_cast<uint8_t>(PickupKind::HEALTH)) ? PickupKind::HEALTH : PickupKind::EMBER;
        respawn_time = reader.readF32();
    }
    static constexpr size_t getSerialisedSize() { return sizeof(uint8_t) + sizeof(float); }
};

// One stop on the route of a sentinel. A sentinel is spawned for every route, and flies round its points in order, for ever
struct PatrolPointComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("PatrolPointComponent");

    uint32_t route{};
    uint32_t order{};

    void serialise(gc::ByteWriter& writer) const
    {
        writer.writeU32(route);
        writer.writeU32(order);
    }
    void deserialise(gc::ByteReader& reader)
    {
        route = reader.readU32();
        order = reader.readU32();
    }
    static constexpr size_t getSerialisedSize() { return 2 * sizeof(uint32_t); }
};

// Turns the entity about an axis, and optionally bobs it up and down. Only for show, so every host does it by itself.
// Used by decorations in the map and by the models of pickups.
struct SpinComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("SpinComponent");

    glm::vec3 axis{0.0f, 0.0f, 1.0f};
    float radians_per_second{1.0f};
    float bob_height{0.0f}; // metres either side of where the entity started
    float bob_speed{2.0f};  // radians per second

    // not saved
    float angle{};
    float bob_angle{};
    float base_z{};
    bool started{false};

    void serialise(gc::ByteWriter& writer) const
    {
        writer.writeF32(axis.x);
        writer.writeF32(axis.y);
        writer.writeF32(axis.z);
        writer.writeF32(radians_per_second);
        writer.writeF32(bob_height);
        writer.writeF32(bob_speed);
    }
    void deserialise(gc::ByteReader& reader)
    {
        axis.x = reader.readF32();
        axis.y = reader.readF32();
        axis.z = reader.readF32();
        radians_per_second = reader.readF32();
        bob_height = reader.readF32();
        bob_speed = reader.readF32();
        // never trust what was read
        const float length = glm::length(axis);
        axis = (length > 1.0e-3f && length < 1.0e3f) ? axis / length : glm::vec3{0.0f, 0.0f, 1.0f};
    }
    static constexpr size_t getSerialisedSize() { return 6 * sizeof(float); }
};

//
// Replicated
//

inline constexpr int32_t PLAYER_MAX_HEALTH = 100;

// On every player, on every host. Decided by the server.
struct PlayerComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("PlayerComponent");

    int32_t health{PLAYER_MAX_HEALTH};
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

//
// Only on the authority, which runs the simulation
//

struct ProjectileComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("ProjectileComponent");

    glm::vec3 velocity{};
    float lifetime{};
    gc::NetPeerId shooter{gc::NET_PEER_NONE};
};

struct PickupComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("PickupComponent");

    PickupKind kind{PickupKind::EMBER};
    uint32_t spawner{}; // which of the map's spawners it came from
};

struct SentinelComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("SentinelComponent");

    uint32_t route{};
    uint32_t next_point{}; // the point on the route that it is flying towards
};

//
// Only on the host that controls the player
//

// A player that walks: see character.h
struct CharacterComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("CharacterComponent");

    glm::vec3 velocity{};
    float yaw{};   // radians. Zero faces +Y, positive turns towards +X
    float pitch{}; // radians. Zero is level, positive looks up
    bool grounded{false};
    bool facing_known{false}; // false until yaw and pitch have been taken from the transform (after spawning, or being moved by the server)
    glm::vec3 last_safe_feet{};
    bool has_safe_feet{false};
};

// On players controlled by other hosts, when rendering. Keeps the model upright while the player looks up and down.
struct PlayerModelComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("PlayerModelComponent");

    gc::Entity model{gc::ENTITY_NONE};
};
