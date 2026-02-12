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
        m_world.forEach<gc::TransformComponent, FollowComponent>([&](gc::Entity, gc::TransformComponent& t, FollowComponent& f) {
            if (f.m_target != gc::ENTITY_NONE) {
                const gc::TransformComponent* const target_t = m_world.getComponent<gc::TransformComponent>(f.m_target);
                if (t.getParent() == target_t->getParent()) {
                    const glm::vec3 follower_to_target = target_t->getPosition() - t.getPosition();
                    const float distance = glm::length(follower_to_target);
                    const glm::vec3 follower_to_target_norm = follower_to_target / distance;
                    t.setRotation(glm::quatLookAtRH(-follower_to_target_norm, glm::vec3{0.0f, 0.0f, 1.0f}) *
                                  glm::angleAxis(-glm::half_pi<float>(), glm::vec3{1.0f, 0.0f, 0.0f}));

                    if (distance < f.m_min_distance) {
                        t.setPosition(t.getPosition() - follower_to_target_norm * (f.m_min_distance - distance));
                    }
                    if (distance < f.m_min_distance + 1.0f) {
                        if (f.m_time_since_contact > f.m_cooldown_seconds) {
                            f.m_time_since_contact = 0.0f;
                        }
                    }
                    else {
                        t.setPosition(t.getPosition() +
                                      follower_to_target_norm * fminf(f.m_speed * static_cast<float>(frame_state.delta_time), distance - f.m_min_distance));
                    }
                    if (f.m_time_since_contact == 0.0f) {
                        const auto ren = m_world.getComponent<gc::RenderableComponent>(f.m_texture_target);
                        if (ren) {
                            const auto& old_material = m_rm.get<gc::ResourceMaterial>(ren->m_material);
                            auto new_material = old_material;
                            new_material.base_color_texture = m_textures[current_texture % m_textures.size()];
                            current_texture += 1;
                            ren->m_material = m_rm.add(std::move(new_material));
                            GC_TRACE("Material switched to: {}", ren->m_material.getString());
                            ren->m_mesh = gc::Name("cube");

                            [[maybe_unused]] auto testname = gc::Name("testtesttest");
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
        if (frame_state.frame_count < 60) {
            return;
        }
        if (!m_loaded) {

            gc::App& app = gc::App::instance();
            gc::ResourceManager& resource_manager = app.resourceManager();
            gc::RenderBackend& render_backend = app.renderBackend();
            gc::Content& content = app.content();
            gc::World& world = app.world();

            render_backend.createPipeline(content.findAsset(gc::Name("fancy.vert"), gcpak::GcpakAssetType::SPIRV_SHADER),
                                          content.findAsset(gc::Name("fancy.frag"), gcpak::GcpakAssetType::SPIRV_SHADER));

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
            world.addComponent<MouseMoveComponent>(camera).setMoveSpeed(25.0f).setAcceleration(40.0f).setDeceleration(100.0f).setSensitivity(1e-3f);
            world.addComponent<gc::LightComponent>(camera);

            const auto shrek_parent = world.createEntity(gc::Name("shrek_parent"), gc::ENTITY_NONE, glm::vec3{0.0f, +100.0f, 5.0f});
            world.addComponent<FollowComponent>(shrek_parent).setTarget(camera).setMinDistance(5.0f).setSpeed(10.0f);

            const auto shrek = world.createEntity(gc::Name("shrek"), shrek_parent, glm::vec3{0.0f, +0.0f, -4.331f});
            world.addComponent<gc::RenderableComponent>(shrek).setMaterial(gc::Name("default_material")).setMesh(gc::Name("shrek.obj"));

            world.getComponent<FollowComponent>(shrek_parent)->setTextureTarget(shrek);

            // but cannot load materials from disk yet, so create manually:
            {
                gc::ResourceMaterial default_material{};
                default_material.base_color_texture = gc::Name("bricks-mortar-albedo.png");
                default_material.occlusion_roughness_metallic_texture = gc::Name("bricks-mortar-orm.png");
                default_material.normal_texture = gc::Name("bricks-mortar-normal.png");
                resource_manager.add<gc::ResourceMaterial>(std::move(default_material), gc::Name("default_material"));
            }

            std::array<gc::Entity, 36> cubes{};
            const gc::Entity parent = world.createEntity(gc::Name("parent"), gc::ENTITY_NONE, glm::vec3{0.0f, 15.0f, 5.5f});
            world.addComponent<SpinComponent>(parent).setAxis({0.3f, 0.4f, 1.0f}).setRadiansPerSecond(0.1f);

            for (int x = 0; x < 6; ++x) {
                for (int y = 0; y < 6; ++y) {
                    auto& cube = cubes[x * 6 + y];
                    cube = world.createEntity(gc::Name(std::format("cube{}.{}", x, y)), parent, glm::vec3{(x - 2.5f) * 2.0f, 0.0f, (y - 2.5f) * 2.0f});
                    world.addComponent<gc::RenderableComponent>(cube).setMesh(gc::Name("sphere")).setMaterial(gc::Name("default_material"));
                    world.addComponent<SpinComponent>(cube).setAxis({1.0f, 0.0f, 0.7f}).setRadiansPerSecond(0.0f);
                }
            }

            // add a floor
            const auto floor = world.createEntity(gc::Name("floor"), gc::ENTITY_NONE, {0.0f, 0.0f, -0.5f}, {}, {100.0f, 100.0f, 1.0f});
            world.addComponent<gc::RenderableComponent>(floor).setMesh(gc::Name("cube")).setMaterial(gc::Name("default_material"));

            resource_manager.add<gc::ResourceMesh>(genCuboidMesh(1.0f, 1.0f, 1.0f), gc::Name("cube"));
            resource_manager.add<gc::ResourceMesh>(genSphereMesh(1.0f, 10), gc::Name("sphere"));

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
        app.renderBackend().setSyncMode(gc::RenderSyncMode::VSYNC_ON_TRIPLE_BUFFERED_UNTHROTTLED);
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
