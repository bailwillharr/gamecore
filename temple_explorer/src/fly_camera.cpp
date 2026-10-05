#include "fly_camera.h"

#include <cmath>

#include <format>
#include <limits>
#include <unordered_map>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <imgui.h>

#include <tracy/Tracy.hpp>

#include <gamecore/gc_app.h>
#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_collision_system.h>
#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_light_component.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_resource_manager.h>
#include <gamecore/gc_resources.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_world.h>

void addHeadlamp(gc::World& world, gc::Entity entity)
{
    // Worlds can be as bright as day or as dim as a cave, and a lamp of any one strength would be blinding or invisible in most
    // of them. Make it as strong as the camera's exposure expects: a surface 5 metres away is lit as well as the world's main
    // light would light it (see CameraComponent::setExposure()).
    const gc::CameraComponent* const camera = world.getComponent<gc::CameraComponent>(entity);
    const float illuminance = 3.0f * std::exp2(camera ? camera->getExposure() : 15.0f); // lux
    const float intensity = illuminance * 5.0f * 5.0f;                                  // candela
    world.addComponent<gc::LightComponent>(entity).setType(gc::LightType::POINT).setColor({1.0f, 0.9f, 0.75f}).setIntensity(intensity).setRange(40.0f);
}

FlyCameraSystem::FlyCameraSystem(gc::World& world) : gc::System(world) {}

void FlyCameraSystem::onUpdate(gc::FrameState& frame_state)
{
    ZoneScoped;

    if (!m_framed) {
        // Done on the first update rather than when the world is loaded, as world positions are only known once the
        // TransformSystem has run.
        frameWorld();
        m_framed = true;
    }

    const gc::WindowState& input = *frame_state.window_state;
    const float delta_time = static_cast<float>(frame_state.delta_time);

    if (input.getKeyPress(SDL_SCANCODE_ESCAPE)) {
        gc::app().requestQuit();
    }

    // The mouse isn't captured while the debug windows are open (F10). Don't look around then.
    const glm::vec2 mouse_motion = input.getIsMouseCaptured() ? input.getMouseMotion() : glm::vec2{0.0f, 0.0f};

    glm::vec3 move{0.0f, 0.0f, 0.0f}; // right, forward, up
    move.x += input.getKeyDown(SDL_SCANCODE_D) ? 1.0f : 0.0f;
    move.x -= input.getKeyDown(SDL_SCANCODE_A) ? 1.0f : 0.0f;
    move.y += input.getKeyDown(SDL_SCANCODE_W) ? 1.0f : 0.0f;
    move.y -= input.getKeyDown(SDL_SCANCODE_S) ? 1.0f : 0.0f;
    move.z += (input.getKeyDown(SDL_SCANCODE_SPACE) || input.getKeyDown(SDL_SCANCODE_E)) ? 1.0f : 0.0f;
    move.z -= (input.getKeyDown(SDL_SCANCODE_LCTRL) || input.getKeyDown(SDL_SCANCODE_Q)) ? 1.0f : 0.0f;
    const bool fast = input.getKeyDown(SDL_SCANCODE_LSHIFT);

    float speed_change = 1.0f;
    if (input.getKeyPress(SDL_SCANCODE_UP)) {
        speed_change = 2.0f;
    }
    else if (input.getKeyPress(SDL_SCANCODE_DOWN)) {
        speed_change = 0.5f;
    }

    const bool toggle_headlamp = input.getKeyPress(SDL_SCANCODE_L);
    const bool toggle_collide = input.getKeyPress(SDL_SCANCODE_C);
    gc::Entity headlamp_entity = gc::ENTITY_NONE;

    m_world.forEach<gc::TransformComponent, FlyCameraComponent>([&](gc::Entity entity, gc::TransformComponent& t, FlyCameraComponent& camera) {
        camera.speed = glm::clamp(camera.speed * speed_change, 0.01f, 10000.0f);

        camera.yaw += mouse_motion.x * camera.sensitivity;
        camera.pitch = glm::clamp(camera.pitch + mouse_motion.y * camera.sensitivity, 0.0f, glm::pi<float>());

        // Cameras look along their -Z axis, with +Y up. The world is Z-up.
        const glm::quat rotation = glm::angleAxis(-camera.yaw, glm::vec3{0.0f, 0.0f, 1.0f}) * glm::angleAxis(camera.pitch, glm::vec3{1.0f, 0.0f, 0.0f});
        const glm::vec3 forward = rotation * glm::vec3{0.0f, 0.0f, -1.0f};
        const glm::vec3 right = rotation * glm::vec3{1.0f, 0.0f, 0.0f};

        // W and S fly where the camera is looking, including up and down. The up and down keys are always along the world's Z.
        glm::vec3 wanted_velocity = move.x * right + move.y * forward + move.z * glm::vec3{0.0f, 0.0f, 1.0f};
        if (glm::dot(wanted_velocity, wanted_velocity) > 0.0f) {
            wanted_velocity = glm::normalize(wanted_velocity) * camera.speed * (fast ? camera.fast_multiplier : 1.0f);
        }
        // ease towards it, so that starting and stopping isn't abrupt
        const float blend = (camera.smoothing_time > 0.0f) ? 1.0f - std::exp(-delta_time / camera.smoothing_time) : 1.0f;
        camera.velocity += (wanted_velocity - camera.velocity) * blend;

        if (toggle_collide) {
            camera.collide = !camera.collide;
        }
        glm::vec3 position = t.getPosition() + camera.velocity * delta_time;
        if (camera.collide) {
            position = collideWithWorld(camera, entity, position, camera.velocity);
        }

        t.setRotation(rotation);
        t.setPosition(position);

        // what the camera is looking at
        const gc::RaycastHit hit = m_world.getSystem<gc::CollisionSystem>().raycast(gc::Ray{position, forward}, 10000.0f, entity);
        if (hit.hit) {
            const gc::TransformComponent* const hit_transform = m_world.getComponent<gc::TransformComponent>(hit.entity);
            m_looking_at = std::format("{} ({:.1f} m)", hit_transform ? hit_transform->name.getString() : "?", hit.distance);
        }
        else {
            m_looking_at = "nothing";
        }

        headlamp_entity = entity;
    });

    if (headlamp_entity != gc::ENTITY_NONE) {
        // The headlamp is a point light on the camera, on top of whatever lights the world has
        bool headlamp = m_world.getComponent<gc::LightComponent>(headlamp_entity) != nullptr;
        if (toggle_headlamp) {
            if (headlamp) {
                m_world.removeComponent<gc::LightComponent>(headlamp_entity);
            }
            else {
                addHeadlamp(m_world, headlamp_entity);
            }
            headlamp = !headlamp;
        }
        showHelp(*m_world.getComponent<FlyCameraComponent>(headlamp_entity), m_world.getComponent<gc::TransformComponent>(headlamp_entity)->getPosition(),
                 headlamp);
    }
}

glm::vec3 FlyCameraSystem::collideWithWorld(const FlyCameraComponent& camera, gc::Entity entity, glm::vec3 position, glm::vec3& velocity)
{
    const gc::CollisionSystem& collision = m_world.getSystem<gc::CollisionSystem>();

    // Move out of the deepest contact, then look again, as that can push the camera into something else (in a corner).
    std::vector<gc::SphereContact> contacts{};
    for (int iteration = 0; iteration < 4; ++iteration) {
        contacts.clear();
        collision.overlapSphere(position, camera.radius, contacts, entity);
        const gc::SphereContact* deepest = nullptr;
        for (const gc::SphereContact& contact : contacts) {
            if (!deepest || contact.depth > deepest->depth) {
                deepest = &contact;
            }
        }
        if (!deepest) {
            break;
        }
        position += deepest->normal * deepest->depth;
        // keep the part of the velocity that slides along the surface
        const float into_surface = glm::dot(velocity, deepest->normal);
        if (into_surface < 0.0f) {
            velocity -= deepest->normal * into_surface;
        }
    }
    return position;
}

void FlyCameraSystem::frameWorld()
{
    glm::vec3 bounds_min{std::numeric_limits<float>::max()};
    glm::vec3 bounds_max{std::numeric_limits<float>::lowest()};
    bool found = false;
    // The bounds of everything that is drawn. The positions of the entities aren't enough, as a mesh can be any size and
    // anywhere relative to its entity. This reads every mesh in the world once (meshes are views of the mapped .gcpak file).
    gc::ResourceManager& resource_manager = gc::app().resourceManager();
    struct MeshBounds {
        glm::vec3 min;
        glm::vec3 max;
    };
    std::unordered_map<gc::Name, MeshBounds> mesh_bounds{}; // a mesh is often used by many entities
    m_world.forEach<gc::TransformComponent, gc::RenderableComponent>([&](gc::Entity, const gc::TransformComponent& t, const gc::RenderableComponent& r) {
        if (r.m_mesh.empty()) {
            return;
        }
        auto it = mesh_bounds.find(r.m_mesh);
        if (it == mesh_bounds.end()) {
            MeshBounds bounds{glm::vec3{std::numeric_limits<float>::max()}, glm::vec3{std::numeric_limits<float>::lowest()}};
            if (const gc::ResourceMesh* const mesh = resource_manager.get<gc::ResourceMesh>(r.m_mesh); mesh) {
                for (const gc::MeshVertex& vertex : mesh->vertices.get()) {
                    bounds.min = glm::min(bounds.min, vertex.position);
                    bounds.max = glm::max(bounds.max, vertex.position);
                }
            }
            it = mesh_bounds.emplace(r.m_mesh, bounds).first;
        }
        const MeshBounds& bounds = it->second;
        if (bounds.min.x > bounds.max.x) {
            return; // the mesh is missing or empty
        }
        const glm::mat4 matrix = t.getWorldMatrix();
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 local{(corner & 1) ? bounds.max.x : bounds.min.x, (corner & 2) ? bounds.max.y : bounds.min.y,
                                  (corner & 4) ? bounds.max.z : bounds.min.z};
            const glm::vec3 position = glm::vec3(matrix * glm::vec4(local, 1.0f));
            bounds_min = glm::min(bounds_min, position);
            bounds_max = glm::max(bounds_max, position);
        }
        found = true;
    });
    if (!found) {
        return;
    }

    const glm::vec3 centre = 0.5f * (bounds_min + bounds_max);
    const glm::vec3 size = bounds_max - bounds_min;
    const float diagonal = glm::max(glm::length(size), 1.0f);

    m_world.forEach<gc::TransformComponent, FlyCameraComponent>([&](gc::Entity, gc::TransformComponent& t, FlyCameraComponent& camera) {
        // in front of the world (on its -Y side), a little above the middle, looking in
        t.setPosition(centre + glm::vec3{0.0f, -0.5f * size.y - 0.15f * diagonal, 0.1f * diagonal});
        camera.yaw = 0.0f;
        camera.pitch = glm::half_pi<float>();
        camera.speed = glm::clamp(diagonal / 20.0f, 1.0f, 100.0f);
    });
}

void FlyCameraSystem::showHelp(const FlyCameraComponent& camera, const glm::vec3& position, bool headlamp)
{
    if (!ImGui::GetCurrentContext()) {
        return;
    }
    constexpr ImGuiWindowFlags FLAGS = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
    // the work area starts below the debug menu bar, when that is shown (F10)
    const ImVec2 work_pos = ImGui::GetMainViewport()->WorkPos;
    ImGui::SetNextWindowPos(ImVec2(work_pos.x + 10.0f, work_pos.y + 10.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.5f);
    if (ImGui::Begin("Fly camera", nullptr, FLAGS)) {
        ImGui::TextUnformatted("Mouse: look   WASD: fly   Space/E: up   Ctrl/Q: down   Shift: fast");
        ImGui::TextUnformatted("Up/Down arrows: change speed   L: headlamp   C: collisions   F10: debug windows   Esc: quit");
        ImGui::Text("Position: %.1f %.1f %.1f   Speed: %.2f m/s   Headlamp: %s   Collisions: %s", position.x, position.y, position.z, camera.speed,
                    headlamp ? "on" : "off", camera.collide ? "on" : "off");
        ImGui::Text("Looking at: %s", m_looking_at.c_str());
    }
    ImGui::End();
}
