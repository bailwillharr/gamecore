#pragma once

#include <cstdint>

#include <glm/vec3.hpp>

#include <gamecore/gc_ecs.h>
#include <gamecore/gc_name.h>

// The person exploring. Their entity's transform is their eyes, with the camera on it
struct ExplorerComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("ExplorerComponent");

    float yaw{};   // radians, clockwise from +y (north)
    float pitch{}; // radians, up from the horizon
    glm::vec3 velocity{};
    bool grounded{false};
    bool flying{false};
    glm::vec3 last_safe_feet{}; // where they last stood, in case they fall out of the world
    bool has_safe_feet{false};
};

enum class BeaconKind : uint8_t {
    DEEPEST, // the lowest point of the fissure
    HIGHEST, // the highest ground in the scan
    SITE,    // somewhere else worth a look
};

// A survey beacon, to be walked (or flown) up to
struct BeaconComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("BeaconComponent");

    BeaconKind kind{BeaconKind::SITE};
    uint32_t number{}; // for the sites, 1, 2, 3...
    bool found{false};
    float found_after{}; // seconds since the start
    gc::Entity lamp{gc::ENTITY_NONE}; // the glowing part at the top, with the light
};
