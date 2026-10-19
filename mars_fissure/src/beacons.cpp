#include "beacons.h"

#include <format>

#include <glm/geometric.hpp>

#include <tracy/Tracy.hpp>

#include <gclog/gclog.h>

#include <gamecore/gc_collider_component.h>
#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_gen_mesh.h>
#include <gamecore/gc_light_component.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_resource_manager.h>
#include <gamecore/gc_resources.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_world.h>

using namespace gc::literals;

static constexpr glm::vec3 LAMP_COLOR{0.3f, 0.85f, 1.0f};
static constexpr glm::vec3 FOUND_COLOR{0.35f, 1.0f, 0.3f};
static constexpr float LAMP_HEIGHT = 3.8f; // above the ground

static gc::ResourceMaterial makeGlowingMaterial(const glm::vec3& emissive)
{
    gc::ResourceMaterial material{};
    material.base_color = glm::vec4{0.02f, 0.02f, 0.02f, 1.0f};
    material.roughness = 1.0f;
    material.emissive = emissive;
    return material;
}

BeaconSystem::BeaconSystem(gc::World& world, gc::ResourceManager& resource_manager, const Sites& sites, gc::Entity explorer)
    : gc::System(world), m_explorer(explorer)
{
    // Resources don't have to come from a .gcpak file: these are made here, under names of the game's own
    resource_manager.add<gc::ResourceMesh>(gc::genCubeMesh(), "mars/cube"_name);
    resource_manager.add<gc::ResourceMesh>(gc::genSphereMesh(16), "mars/sphere"_name);
    {
        gc::ResourceMaterial metal{};
        metal.base_color = glm::vec4{0.55f, 0.56f, 0.58f, 1.0f};
        metal.roughness = 0.35f;
        metal.metallic = 1.0f;
        resource_manager.add<gc::ResourceMaterial>(std::move(metal), "mars/beacon_metal"_name);
    }
    // (emission isn't in physical units: 1 is about as bright as a lit white surface)
    resource_manager.add<gc::ResourceMaterial>(makeGlowingMaterial(LAMP_COLOR * 4.0f), "mars/beacon_lamp"_name);
    resource_manager.add<gc::ResourceMaterial>(makeGlowingMaterial(FOUND_COLOR * 4.0f), "mars/beacon_found"_name);

    const auto add_beacon = [&](const glm::vec3& ground, BeaconKind kind, uint32_t number) {
        const gc::Entity root = world.createEntity("beacon"_name, gc::ENTITY_NONE, ground);
        // a plinth and a mast, which are solid
        const gc::Entity plinth = world.createEntity("plinth"_name, root, glm::vec3{-0.8f, -0.8f, -0.5f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                                                     glm::vec3{1.6f, 1.6f, 0.8f});
        world.addComponent<gc::RenderableComponent>(plinth).setMesh("mars/cube"_name).setMaterial("mars/beacon_metal"_name);
        world.addComponent<gc::ColliderComponent>(plinth).setMesh("mars/cube"_name);
        const gc::Entity mast = world.createEntity("mast"_name, root, glm::vec3{-0.08f, -0.08f, 0.3f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                                                   glm::vec3{0.16f, 0.16f, LAMP_HEIGHT - 0.5f});
        world.addComponent<gc::RenderableComponent>(mast).setMesh("mars/cube"_name).setMaterial("mars/beacon_metal"_name);
        world.addComponent<gc::ColliderComponent>(mast).setMesh("mars/cube"_name);
        // and a lamp on top, which lights the ground around it
        const gc::Entity lamp = world.createEntity("lamp"_name, root, glm::vec3{0.0f, 0.0f, LAMP_HEIGHT}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                                                   glm::vec3{0.3f});
        world.addComponent<gc::RenderableComponent>(lamp).setMesh("mars/sphere"_name).setMaterial("mars/beacon_lamp"_name);
        world.addComponent<gc::LightComponent>(lamp).setType(gc::LightType::POINT).setColor(LAMP_COLOR).setIntensity(600.0f).setRange(60.0f);

        BeaconComponent& beacon = world.addComponent<BeaconComponent>(root);
        beacon.kind = kind;
        beacon.number = number;
        beacon.lamp = lamp;
        ++m_total;
    };
    add_beacon(sites.deepest, BeaconKind::DEEPEST, 0);
    add_beacon(sites.highest, BeaconKind::HIGHEST, 0);
    for (size_t i = 0; i < sites.others.size(); ++i) {
        add_beacon(sites.others[i], BeaconKind::SITE, static_cast<uint32_t>(i + 1));
    }
}

void BeaconSystem::onUpdate(gc::FrameState& frame_state)
{
    ZoneScoped;

    m_time += frame_state.delta_time;
    const gc::TransformComponent* const explorer = m_world.getComponent<gc::TransformComponent>(m_explorer);
    if (!explorer) {
        return;
    }
    const glm::vec3 eyes = explorer->getPosition();

    m_world.forEach<gc::TransformComponent, BeaconComponent>([&](gc::Entity, const gc::TransformComponent& t, BeaconComponent& beacon) {
        if (beacon.found) {
            return;
        }
        // (measured to the mast, at the explorer's height, so that standing on the plinth counts)
        const glm::vec3 base = t.getPosition();
        const glm::vec3 nearest{base.x, base.y, glm::clamp(eyes.z, base.z, base.z + LAMP_HEIGHT)};
        if (glm::distance(eyes, nearest) > REACH_DISTANCE) {
            return;
        }
        beacon.found = true;
        beacon.found_after = static_cast<float>(m_time);
        ++m_found;
        if (gc::RenderableComponent* const lamp = m_world.getComponent<gc::RenderableComponent>(beacon.lamp)) {
            lamp->setMaterial("mars/beacon_found"_name);
        }
        if (gc::LightComponent* const light = m_world.getComponent<gc::LightComponent>(beacon.lamp)) {
            light->setColor(FOUND_COLOR);
        }
        GC_INFO("Found {} after {:.0f} s ({} of {})", getBeaconName(beacon), m_time, m_found, m_total);
        if (m_found == m_total) {
            m_completed_after = m_time;
        }
    });
}

std::string BeaconSystem::getBeaconName(const BeaconComponent& beacon)
{
    switch (beacon.kind) {
    case BeaconKind::DEEPEST:
        return "the deepest point";
    case BeaconKind::HIGHEST:
        return "the highest point";
    default:
        return std::format("site {}", beacon.number);
    }
}
