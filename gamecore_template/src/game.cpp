#include "game.h"

#include <cmath>
#include <memory>
#include <random>
#include <vector>

#include <mio/mmap.hpp>

#include <tracy/Tracy.hpp>

#include <imgui.h>

#include <glm/geometric.hpp>

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
#include <gamecore/gc_transform_system.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_world.h>
#include <gamecore/gc_gen_mesh.h>
#include <gamecore/gc_net.h>
#include <gamecore/gc_net_common.h>

#include "mouse_move.h"
#include "spin.h"

static gc::Entity createRemotePlayer(gc::World& world, gc::Name name)
{
    constexpr float CAMERA_HEIGHT = 67.5f * 25.4e-3f;
    const auto player = world.createEntity(name, gc::ENTITY_NONE);
    const auto player_model = world.createEntity(gc::Name("player_model"), player, {0.0f, 0.0f, -CAMERA_HEIGHT});
    world.getComponent<gc::TransformComponent>(player_model)->setScale(0.360f);
    world.addComponent<gc::RenderableComponent>(player_model).setMaterial(gc::Name()).setMesh(gc::Name("shrek.obj"));
    return player;
}

static float extractYaw(const glm::quat& rotation)
{
    // Camera local forward = -Z in its own space
    glm::vec3 localForward = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 worldForward = rotation * localForward;

    // In your coordinate system:
    //   +X = right
    //   -Y = forward (world)
    //   +Z = up
    //
    // So yaw is the angle in the XY plane.
    // atan2(x, -y) gives the correct convention matching your yaw accumulation.
    return std::atan2(worldForward.x, -worldForward.y);
}

static glm::quat yawToQuaternion(float yaw)
{
    // Matches your existing camera yaw convention
    // Rotates around world +Z (up)
    // Default forward direction (yaw = 0) will be +Y
    return glm::angleAxis(yaw, glm::vec3(0.0f, 0.0f, 1.0f));
}

class ReplicatedPlayerSystem : public gc::System {
    std::unordered_map<gc::Name, uint16_t> m_highest_recv_seq_num{};

public:
    ReplicatedPlayerSystem(gc::World& world) : gc::System(world) {}

    void onUpdate([[maybe_unused]] gc::FrameState& frame_state) override
    {
        for (const auto& net_ev : frame_state.net_events) {
            if (net_ev.type == gc::Name("player_snapshot")) {
                gc::NetByteReader reader(net_ev.data);
                const gc::Name player_name(reader.readU32());
                const uint16_t seq_num = reader.readU16();

                if (gc::seq_diff(m_highest_recv_seq_num[player_name], seq_num) < 0) {
                    m_highest_recv_seq_num[player_name] = seq_num;
                    const float pos_x = reader.readF32();
                    const float pos_y = reader.readF32();
                    const float pos_z = reader.readF32();
                    const float yaw = reader.readF32();
                    gc::Entity player = m_world.findEntity(player_name);
                    if (player == gc::ENTITY_NONE) {
                        player = createRemotePlayer(m_world, player_name);
                    }
                    m_world.getComponent<gc::TransformComponent>(player)->setPosition(pos_x, pos_y, pos_z);
                    m_world.getComponent<gc::TransformComponent>(player)->setRotation(yawToQuaternion(yaw));
                }
            }
        }
    }
};

struct ReplicatablePlayerComponent {
    uint16_t seq_num = UINT16_MAX;
    glm::vec3 old_pos{1.0e6f, 1.0e6f, 1.0e6f};
    float old_yaw{1000.0f};
};

class ReplicatablePlayerSystem : public gc::System {
    gc::Net& m_net;

public:
    ReplicatablePlayerSystem(gc::World& world, gc::Net& net) : gc::System(world), m_net(net) {}

    void onUpdate([[maybe_unused]] gc::FrameState& frame_state) override
    {
        if (m_net.getMode() == gc::NetMode::DISCONNECTED) return;

        m_world.forEach<gc::TransformComponent, ReplicatablePlayerComponent>(
            [&](gc::Entity e, const gc::TransformComponent& t, ReplicatablePlayerComponent& p) {
                const uint32_t name = t.name.getHash();
                const glm::vec3 pos = t.getPosition();
                const float yaw = extractYaw(t.getRotation());

                constexpr float MIN_DISTANCE_CHANGE = 0.05; // meters
                constexpr float MIN_YAW_CHANGE = 0.01;      // radians

                if (glm::distance(p.old_pos, pos) > MIN_DISTANCE_CHANGE || fabsf(p.old_yaw - yaw) > MIN_YAW_CHANGE) {
                    gc::NetEvent ev{};
                    ev.type = gc::Name("player_snapshot");
                    ev.data.resize(sizeof(uint32_t)   // player_name
                                   + sizeof(uint16_t) // seq_num
                                   + sizeof(float)    // pos_x
                                   + sizeof(float)    // pos_y
                                   + sizeof(float)    // pos_z
                                   + sizeof(float));  // yaw
                    gc::NetByteWriter writer(ev.data);
                    writer.writeU32(name);
                    writer.writeU16(p.seq_num);
                    writer.writeF32(pos.x);
                    writer.writeF32(pos.y);
                    writer.writeF32(pos.z);
                    writer.writeF32(yaw);

                    m_net.postEvent(ev);
                    ++p.seq_num;
                }

                p.old_pos = pos;
                p.old_yaw = yaw;
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
                auto vert = content.findAsset(gc::Name("pbr_single_draw.vert"));
                auto frag = content.findAsset(gc::Name("pbr.frag"));
                if (vert.data.empty() || vert.type != gcpak::GcpakAssetType::SPIRV_SHADER || frag.data.empty() ||
                    frag.type != gcpak::GcpakAssetType::SPIRV_SHADER) {
                    gc::abortGame("Failed to find shaders");
                    return;
                }
                render_backend.createMainPipeline(vert.data, frag.data);
            }
            {
                auto vert = content.findAsset(gc::Name("pbr_instanced.vert"));
                auto frag = content.findAsset(gc::Name("pbr.frag"));
                if (vert.data.empty() || vert.type != gcpak::GcpakAssetType::SPIRV_SHADER || frag.data.empty() ||
                    frag.type != gcpak::GcpakAssetType::SPIRV_SHADER) {
                    gc::abortGame("Failed to find shaders");
                    return;
                }
                render_backend.createInstancingPipeline(vert.data, frag.data);
            }

            // Register core engine systems and components
            world.registerComponent<gc::RenderableComponent, gc::ComponentArrayType::DENSE>();
            world.registerComponent<gc::CameraComponent, gc::ComponentArrayType::SPARSE>();
            world.registerComponent<gc::LightComponent, gc::ComponentArrayType::SPARSE>();
            world.registerSystem<gc::RenderSystem>(resource_manager, render_backend);
            world.registerSystem<gc::CameraSystem>();
            world.registerSystem<gc::LightSystem>();

            // register game systems and components
            world.registerComponent<SpinComponent, gc::ComponentArrayType::SPARSE>();
            world.registerComponent<MouseMoveComponent, gc::ComponentArrayType::SPARSE>();
            world.registerComponent<ReplicatablePlayerComponent, gc::ComponentArrayType::SPARSE>();
            world.registerSystem<SpinSystem>();
            world.registerSystem<MouseMoveSystem>();
            world.registerSystem<ReplicatedPlayerSystem>();
            world.registerSystem<ReplicatablePlayerSystem>(app.net());

            {
                // player
                std::random_device rd{};
                const gc::Name player_name(std::format("player{}", rd()));
                constexpr float CAMERA_HEIGHT = 67.5f * 25.4e-3f;
                auto player = world.createEntity(player_name, gc::ENTITY_NONE, {0.0f, 0.0f, CAMERA_HEIGHT});
                world.addComponent<gc::CameraComponent>(player).setFOV(glm::radians(45.0f)).setNearPlane(0.1f).setActive(true);
                world.addComponent<MouseMoveComponent>(player).setMoveSpeed(10.0f).setAcceleration(100.0f).setDeceleration(100.0f).setSensitivity(1e-3f);
                world.addComponent<ReplicatablePlayerComponent>(player);
            }

            {
                // light
                world.createEntity(gc::Name("light"), gc::ENTITY_NONE, {0.0f, 0.0f, 3.0f});
            }

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
                resource_manager.add<gc::ResourceMesh>(gc::genPlaneMesh(10.0f, 10.0f), gc::Name("floor"));
                resource_manager.add<gc::ResourceMesh>(gc::genPlaneMesh(10.0f, 4.0f), gc::Name("wall1"));
            }

            // add a floor
            {
                const auto floor = world.createEntity(gc::Name("floor"));
                world.getComponent<gc::TransformComponent>(floor)->setScale({10.0f, 10.0f, 1.0f});
                world.addComponent<gc::RenderableComponent>(floor).setMesh(gc::Name("floor")).setMaterial(gc::Name("laminate-flooring-brown"));
            }

            // wall1
            {
                const auto wall1 = world.createEntity(gc::Name("wall1"));
                world.getComponent<gc::TransformComponent>(wall1)->setPosition({-5.0f, 0.0f, 2.0f});
                world.getComponent<gc::TransformComponent>(wall1)->setScale({10.0f, 4.0f, 1.0f});
                world.getComponent<gc::TransformComponent>(wall1)->setRotation(glm::quat(0.5f, 0.5f, 0.5f, 0.5f));
                world.addComponent<gc::RenderableComponent>(wall1).setMaterial(gc::Name("bricks-mortar")).setMesh(gc::Name("wall1"));
            }

            // wall2
            {
                const auto wall2 = world.createEntity(gc::Name("wall2"));
                world.getComponent<gc::TransformComponent>(wall2)->setPosition({5.0f, 0.0f, 2.0f});
                world.getComponent<gc::TransformComponent>(wall2)->setScale({10.0f, 4.0f, 1.0f});
                world.getComponent<gc::TransformComponent>(wall2)->setRotation(glm::quat(0.5f, 0.5f, -0.5f, -0.5f));
                world.addComponent<gc::RenderableComponent>(wall2).setMaterial(gc::Name("bricks-mortar")).setMesh(gc::Name("wall1"));
            }

            // wall3
            {
                const auto wall3 = world.createEntity(gc::Name("wall3"));
                world.getComponent<gc::TransformComponent>(wall3)->setPosition({0.0f, 5.0f, 2.0f});
                world.getComponent<gc::TransformComponent>(wall3)->setScale({10.0f, 4.0f, 1.0f});
                world.getComponent<gc::TransformComponent>(wall3)->setRotation(
                    glm::quat(0.0f, 0.0f, glm::one_over_root_two<float>(), -glm::one_over_root_two<float>()));
                world.addComponent<gc::RenderableComponent>(wall3).setMaterial(gc::Name("bricks-mortar")).setMesh(gc::Name("wall1"));
            }

            // wall4
            {
                const auto wall3 = world.createEntity(gc::Name("wall3"));
                world.getComponent<gc::TransformComponent>(wall3)->setPosition({0.0f, -5.0f, 2.0f});
                world.getComponent<gc::TransformComponent>(wall3)->setScale({10.0f, 4.0f, 1.0f});
                world.getComponent<gc::TransformComponent>(wall3)->setRotation(
                    glm::quat(0.0f, 0.0f, -glm::one_over_root_two<float>(), -glm::one_over_root_two<float>()));
                world.addComponent<gc::RenderableComponent>(wall3).setMaterial(gc::Name("bricks-mortar")).setMesh(gc::Name("wall1"));
            }

            // roof
            {
                using namespace gc::literals;
                const auto roof = world.createEntity("roof"_name);
                world.getComponent<gc::TransformComponent>(roof)->setPosition(0, 0, 4).setRotation(0, -1, 0, 0).setScale(10, 10, 1);
                world.addComponent<gc::RenderableComponent>(roof).setMesh("floor"_name);
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
    app.debugUI().active = true;
    app.window().setMouseCaptured(false);

    gc::World& world = app.world();
    world.registerSystem<WorldLoadSystem>();

    app.window().setWindowVisibility(true);

    app.run();
}
