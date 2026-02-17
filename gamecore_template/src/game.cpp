#include "game.h"

#include <memory>

#include <mio/mmap.hpp>

#include <tracy/Tracy.hpp>

#include <gamecore/gc_app.h>
#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_camera_system.h>
#include <gamecore/gc_content.h>
#include <gamecore/gc_debug_ui.h>
#include <gamecore/gc_ecs.h>
#include <gamecore/gc_gpu_resources.h>
#include <gamecore/gc_light_component.h>
#include <gamecore/gc_light_system.h>
#include <gamecore/gc_name.h>
#include <gamecore/gc_render_backend.h>
#include <gamecore/gc_render_system.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_resource_manager.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_world.h>

#include "gen_mesh.h"
#include "mouse_move.h"
#include "spin.h"

class FollowSystem; // forward-dec

class FollowComponent {
    friend class FollowSystem;

    gc::Entity m_target{gc::ENTITY_NONE};
    float m_speed = 1.0f;
    float m_min_distance = 1.0f;
    float m_cooldown_seconds = 1.0f;
    gc::Entity m_texture_target{gc::ENTITY_NONE};

    float m_time_since_contact = std::numeric_limits<float>::max();

public:
    FollowComponent& setTarget(gc::Entity target)
    {
        m_target = target;
        return *this;
    }
    FollowComponent& setSpeed(float speed)
    {
        m_speed = speed;
        return *this;
    }
    FollowComponent& setMinDistance(float min_distance)
    {
        m_min_distance = min_distance;
        return *this;
    }
    FollowComponent& setCooldownSeconds(float cooldown_seconds)
    {
        m_cooldown_seconds = cooldown_seconds;
        return *this;
    }
    FollowComponent& setTextureTarget(gc::Entity texture_target)
    {
        m_texture_target = texture_target;
        return *this;
    }
};

class FollowSystem : public gc::System {
    gc::ResourceManager& m_rm;

    int current_texture = 0;
    const std::array<gc::Name, 7> m_textures{gc::Name("box.jpg"),  gc::Name("bricks.jpg"),  gc::Name("fire.jpg"),    gc::Name("nuke.jpg"),
                                             gc::Name("moss.png"), gc::Name("uvcheck.png"), gc::Name("8k_earth.jpg")};

public:
    FollowSystem(gc::World& world, gc::ResourceManager& rm) : gc::System(world), m_rm(rm) {}

    void onUpdate(gc::FrameState& frame_state) override
    {
        ZoneScoped;

        constexpr float EPSILON = 0.001f;

        m_world.forEach<gc::TransformComponent, FollowComponent>([&](gc::Entity, gc::TransformComponent& t, FollowComponent& f) {
            if (f.m_target != gc::ENTITY_NONE) {
                const gc::TransformComponent* const target_t = m_world.getComponent<gc::TransformComponent>(f.m_target);
                if (t.getParent() == target_t->getParent()) {
                    const glm::vec3 follower_to_target = target_t->getPosition() - t.getPosition();
                    const float distance = glm::length(follower_to_target);
                    const float planar_distance = glm::length(glm::vec3{follower_to_target.x, follower_to_target.y, 0.0f});
                    const glm::vec3 follower_to_target_planar_norm = glm::normalize(glm::vec3{follower_to_target.x, follower_to_target.y, 0.0f});
                    t.setRotation(glm::quatLookAtRH(-follower_to_target_planar_norm, glm::vec3{0.0f, 0.0f, 1.0f}) *
                                  glm::angleAxis(-glm::half_pi<float>(), glm::vec3{1.0f, 0.0f, 0.0f}));

                    if (planar_distance < f.m_min_distance - EPSILON) {
                        t.setPosition(t.getPosition() - follower_to_target_planar_norm * (f.m_min_distance - planar_distance));
                    }
                    else if (planar_distance > f.m_min_distance + EPSILON) {
                        t.setPosition(t.getPosition() + follower_to_target_planar_norm *
                                                            fminf(f.m_speed * static_cast<float>(frame_state.delta_time), distance - f.m_min_distance));
                    }

                    if (distance < f.m_min_distance + 1.0f) {
                        if (f.m_time_since_contact > f.m_cooldown_seconds) {
                            f.m_time_since_contact = 0.0f;
                        }
                    }

                    if (f.m_time_since_contact == 0.0f) {
                        const auto ren = m_world.getComponent<gc::RenderableComponent>(f.m_texture_target);
                        if (ren) {
                            const auto old_material = m_rm.get<gc::ResourceMaterial>(ren->m_material);
                            gc::ResourceMaterial new_material{};
                            if (old_material) {
                                new_material = *old_material;
                            }
                            new_material.base_color_texture = m_textures[current_texture % m_textures.size()];
                            current_texture += 1;
                            ren->m_material = m_rm.add(std::move(new_material));
                            GC_TRACE("Material switched to: {}", ren->m_material.getString());
                        }
                        else {
                            const auto texture_target_t = m_world.getComponent<gc::TransformComponent>(f.m_texture_target);
                            GC_WARN_ONCE("FollowComponent of entity '{}' has texture target '{}' with no RenderableComponent!", t.name.getString(),
                                         texture_target_t ? texture_target_t->name.getString() : std::string("ENTITY_NONE"));
                        }
                    }
                    f.m_time_since_contact += static_cast<float>(frame_state.delta_time);
                }
                else {
                    GC_WARN_ONCE("FollowComponent of entity '{}' has target '{}' with a different parent!", t.name.getString(), target_t->name.getString());
                }
            }
        });
    }
};

class WorldLoadSystem : public gc::System {
    bool m_loaded = false;
    std::unique_ptr<gc::RenderMaterial> m_fallback_material{};
    std::unique_ptr<gc::RenderMaterial> m_skybox_material{};
    std::unique_ptr<gc::RenderMaterial> m_floor_material{};
    static const std::array<gc::Name, 7> s_texture_names;
    std::array<std::unique_ptr<gc::RenderMaterial>, s_texture_names.size()> m_materials{};
    std::array<std::unique_ptr<gc::RenderMesh>, 4> m_meshes{};

public:
    WorldLoadSystem(gc::World& world) : gc::System(world) {}

    void onUpdate([[maybe_unused]] gc::FrameState& frame_state) override
    {
        if (!m_loaded) {

            gc::App& app = gc::App::instance();
            gc::ResourceManager& resource_manager = app.resourceManager();
            gc::RenderBackend& render_backend = app.renderBackend();
            gc::Content& content = app.content();
            gc::World& world = app.world();
            {
                auto vert = content.findAsset(gc::Name("fancy.vert"), gcpak::GcpakAssetType::SPIRV_SHADER);
                auto frag = content.findAsset(gc::Name("fancy.frag"), gcpak::GcpakAssetType::SPIRV_SHADER);
                if (vert.empty() || frag.empty()) {
                    GC_ERROR_ONCE("Could not find fancy.vert or fancy.frag. Cannot load game.");
                    return;
                }
                render_backend.createPipeline(vert, frag);
            }

            world.registerComponent<gc::RenderableComponent, gc::ComponentArrayType::DENSE>();
            world.registerComponent<gc::CameraComponent, gc::ComponentArrayType::SPARSE>();
            world.registerComponent<gc::LightComponent, gc::ComponentArrayType::SPARSE>();

            world.registerSystem<gc::RenderSystem>(resource_manager, render_backend);
            world.registerSystem<gc::CameraSystem>();
            world.registerSystem<gc::LightSystem>();

            world.registerComponent<SpinComponent, gc::ComponentArrayType::SPARSE>();
            world.registerComponent<MouseMoveComponent, gc::ComponentArrayType::SPARSE>();
            world.registerComponent<FollowComponent, gc::ComponentArrayType::SPARSE>();

            world.registerSystem<SpinSystem>();
            world.registerSystem<MouseMoveSystem>();
            world.registerSystem<FollowSystem>(resource_manager);

            // camera
            auto camera = world.createEntity(gc::Name("light"), gc::ENTITY_NONE, {0.0f, 0.0f, 67.5f * 25.4e-3f});
            world.addComponent<gc::CameraComponent>(camera).setFOV(glm::radians(45.0f)).setNearPlane(0.1f).setActive(true);
            world.addComponent<MouseMoveComponent>(camera).setMoveSpeed(3.0f).setAcceleration(40.0f).setDeceleration(100.0f).setSensitivity(1e-3f);

            // shrek
            const auto shrek = world.createEntity(gc::Name("shrek"), gc::ENTITY_NONE, glm::vec3{0.0f, +10.0f, 0.0f}, {}, {0.5f, 0.5f, 0.5f});
            world.addComponent<gc::RenderableComponent>(shrek).setMaterial(gc::Name("default_material")).setMesh(gc::Name("shrek.obj"));
            world.addComponent<FollowComponent>(shrek).setTarget(camera).setMinDistance(2.0f).setSpeed(1.0f);
            world.getComponent<FollowComponent>(shrek)->setTextureTarget(shrek);
            const auto shrek_light = world.createEntity(gc::Name("shrek_light"), shrek, {0.0f, -1.26688, 4.61091});
            world.addComponent<gc::LightComponent>(shrek_light);

            {
                gc::ResourceMaterial material{};
                material.base_color_texture = gc::Name("bricks-mortar-albedo.png");
                material.orm_texture = gc::Name("bricks-mortar-orm.png");
                material.normal_texture = gc::Name("bricks-mortar-normal.png");
                resource_manager.add<gc::ResourceMaterial>(std::move(material), gc::Name("bricks-mortar"));
            }
            {
                gc::ResourceMaterial material{};
                material.base_color_texture = gc::Name("laminate-flooring-brown_albedo.png");
                material.orm_texture = gc::Name("laminate-flooring-brown_orm.png");
                material.normal_texture = gc::Name("laminate-flooring-brown_normal.png");
                resource_manager.add<gc::ResourceMaterial>(std::move(material), gc::Name("laminate-flooring-brown"));
            }
            {
                resource_manager.add<gc::ResourceMesh>(genPlaneMesh(6.0f, 10.0f), gc::Name("floor"));
                resource_manager.add<gc::ResourceMesh>(genPlaneMesh(10.0f, 4.0f), gc::Name("wall1"));
                resource_manager.add<gc::ResourceMesh>(genPlaneMesh(6.0f, 4.0f), gc::Name("wall3"));
            }

            // add a floor
            const auto floor = world.createEntity(gc::Name("floor"));
            world.getComponent<gc::TransformComponent>(floor)->setScale({6.0f, 10.0f, 1.0f});
            world.addComponent<gc::RenderableComponent>(floor).setMesh(gc::Name("floor")).setMaterial(gc::Name("laminate-flooring-brown"));

            // wall1
            {
                const auto wall1 = world.createEntity(gc::Name("wall1"));
                world.getComponent<gc::TransformComponent>(wall1)->setPosition({-3.0f, 0.0f, 2.0f});
                world.getComponent<gc::TransformComponent>(wall1)->setScale({10.0f, 4.0f, 1.0f});
                world.getComponent<gc::TransformComponent>(wall1)->setRotation(glm::quat(0.5f, 0.5f, 0.5f, 0.5f));
                world.addComponent<gc::RenderableComponent>(wall1).setMaterial(gc::Name("bricks-mortar")).setMesh(gc::Name("wall1"));
            }

            // wall2
            {
                const auto wall2 = world.createEntity(gc::Name("wall2"));
                world.getComponent<gc::TransformComponent>(wall2)->setPosition({3.0f, 0.0f, 2.0f});
                world.getComponent<gc::TransformComponent>(wall2)->setScale({10.0f, 4.0f, 1.0f});
                world.getComponent<gc::TransformComponent>(wall2)->setRotation(glm::quat(0.5f, 0.5f, -0.5f, -0.5f));
                world.addComponent<gc::RenderableComponent>(wall2).setMaterial(gc::Name("bricks-mortar")).setMesh(gc::Name("wall1"));
            }

            // wall3
            {
                const auto wall3 = world.createEntity(gc::Name("wall3"));
                world.getComponent<gc::TransformComponent>(wall3)->setPosition({0.0f, 5.0f, 2.0f});
                world.getComponent<gc::TransformComponent>(wall3)->setScale({6.0f, 4.0f, 1.0f});
                world.getComponent<gc::TransformComponent>(wall3)->setRotation(
                    glm::quat(0.0f, 0.0f, glm::one_over_root_two<float>(), -glm::one_over_root_two<float>()));
                world.addComponent<gc::RenderableComponent>(wall3).setMaterial(gc::Name("bricks-mortar")).setMesh(gc::Name("wall1"));
            }

            // wall4
            {
                const auto wall3 = world.createEntity(gc::Name("wall3"));
                world.getComponent<gc::TransformComponent>(wall3)->setPosition({0.0f, -5.0f, 2.0f});
                world.getComponent<gc::TransformComponent>(wall3)->setScale({6.0f, 4.0f, 1.0f});
                world.getComponent<gc::TransformComponent>(wall3)->setRotation(
                    glm::quat(0.0f, 0.0f, -glm::one_over_root_two<float>(), -glm::one_over_root_two<float>()));
                world.addComponent<gc::RenderableComponent>(wall3).setMaterial(gc::Name("bricks-mortar")).setMesh(gc::Name("wall1"));
            }

            m_loaded = true;
        }
    }
};

void buildAndStartGame(gc::App& app, Options options)
{
    if (options.render_sync_mode.has_value()) {
        app.renderBackend().setSyncMode(static_cast<gc::RenderSyncMode>(options.render_sync_mode.value()));
    }
    else {
        // On Windows/NVIDIA, TRIPLE_BUFFERED gives horrible latency and TRIPLE_BUFFERED_UNTHROTTLED doesn't work properly so use double buffering instead
#ifdef WIN32
        app.renderBackend().setSyncMode(gc::RenderSyncMode::VSYNC_ON_DOUBLE_BUFFERED);
#else
        app.renderBackend().setSyncMode(gc::RenderSyncMode::VSYNC_ON_DOUBLE_BUFFERED);
#endif
    }

    app.window().setTitle("Hello world!");
    app.window().setIsResizable(true);
    app.window().setMouseCaptured(true);
    // app.window().setSize(0, 0, true);
    app.window().setWindowVisibility(true);

    // app.debugUI().active = true;

    gc::World& world = app.world();
    world.registerSystem<WorldLoadSystem>();
    app.run();
}
