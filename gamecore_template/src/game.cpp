#include "game.h"

#include "gamecore/gc_ecs.h"
#include "gen_mesh.h"
#include "spin.h"
#include "mouse_move.h"

#include <mio/mmap.hpp>

#include <tracy/Tracy.hpp>

#include <gamecore/gc_app.h>
#include <gamecore/gc_render_backend.h>
#include <gamecore/gc_world.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_content.h>
#include <gamecore/gc_cube_component.h>
#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_debug_ui.h>

class FollowSystem; // forward-dec

class FollowComponent {
    friend class FollowSystem;

    gc::Entity m_target{gc::ENTITY_NONE};
    float m_speed = 1.0f;
    float m_min_distance = 1.0f;

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
};

class FollowSystem : public gc::System {
public:
    FollowSystem(gc::World& world) : gc::System(world) {}

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
                    else {
                        t.setPosition(t.getPosition() +
                                      follower_to_target_norm * fminf(f.m_speed * static_cast<float>(frame_state.delta_time), distance - f.m_min_distance));
                    }
                }
            }
        });
    }
};

static gc::RenderTexture createORMTexture(gc::RenderBackend& render_backend, float roughness, float metallic)
{
    std::array<uint8_t, 4 + 4 + 4> image_data{};              // uint32 width, uint32 height, and one RGBA pixel
    image_data[0] = 1u;                                       // width = 1
    image_data[4] = 1u;                                       // height = 1
    image_data[8] = 255u;                                     // R channel occlusion
    image_data[9] = static_cast<uint8_t>(roughness * 255.0f); // G channel roughness
    image_data[10] = static_cast<uint8_t>(metallic * 255.0f); // B channel metallic
    image_data[11] = 0u;                                      // unused alpha channel
    return render_backend.createTexture(image_data, false);
}

static gc::RenderTexture createNormalTexture(gc::RenderBackend& render_backend)
{
    std::array<uint8_t, 4 + 4 + 4> image_data{}; // uint32 width, uint32 height, and one RGBA pixel
    image_data[0] = 1u;                          // width = 1
    image_data[4] = 1u;                          // height = 1
    image_data[8] = 128u;                        // R channel (tangent space X)
    image_data[9] = 128u;                        // G channel (tangent space Y)
    image_data[10] = 255u;                       // B channel (tangent space Z)
    image_data[11] = 0u;                         // unused alpha channel
    return render_backend.createTexture(image_data, false);
}

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

    void onUpdate(gc::FrameState& frame_state) override
    {
        if (!m_loaded) {

            gc::App& app = gc::App::instance();
            gc::RenderBackend& render_backend = app.renderBackend();
            gc::Content& content = app.content();
            gc::World& world = app.world();

            auto pipeline = std::make_shared<gc::GPUPipeline>(
                render_backend.createPipeline(content.findAsset(gc::Name("fancy.vert"), gcpak::GcpakAssetType::SPIRV_SHADER),
                                              content.findAsset(gc::Name("fancy.frag"), gcpak::GcpakAssetType::SPIRV_SHADER)));

            {
                auto orm_texture = [&] {
                    std::array<uint8_t, 4 + 4 + 4> image_data{};         // uint32 width, uint32 height, and one RGBA pixel
                    image_data[0] = 1u;                                  // width = 1
                    image_data[4] = 1u;                                  // height = 1
                    image_data[8] = 255u;                                // R channel occlusion
                    image_data[9] = static_cast<uint8_t>(0.5 * 255.0f);  // G channel roughness
                    image_data[10] = static_cast<uint8_t>(0.0 * 255.0f); // B channel metallic
                    image_data[11] = 0u;                                 // unused alpha channel
                    return std::make_shared<gc::RenderTexture>(render_backend.createTexture(image_data, false));
                }();
                auto normal_texture = [&] {
                    std::array<uint8_t, 4 + 4 + 4> image_data{}; // uint32 width, uint32 height, and one RGBA pixel
                    image_data[0] = 1u;                          // width = 1
                    image_data[4] = 1u;                          // height = 1
                    image_data[8] = 128u;                        // R channel (tangent space X)
                    image_data[9] = 128u;                        // G channel (tangent space Y)
                    image_data[10] = 255u;                       // B channel (tangent space Z)
                    image_data[11] = 0u;                         // unused alpha channel
                    return std::make_shared<gc::RenderTexture>(render_backend.createTexture(image_data, false));
                }();
                auto base_color_texture = [&] {
                    constexpr uint32_t SIZE = 4;
                    std::array<uint8_t, 8 + 4 * SIZE * SIZE> image_data{}; // uint32 width, uint32 height, and four RGBA pixels
                    static_assert(SIZE <= 255);
                    image_data[0] = SIZE;                                  // width
                    image_data[4] = SIZE;                                  // height
                    for (size_t y = 0; y < SIZE; ++y) {
                        for (size_t x = 0; x < SIZE; ++x) {
                            image_data[8 + (y * SIZE * 4) + x * 4 + 0] = ((x + y) % 2 == 0) ? 255u : 0u; // R channel
                            image_data[8 + (y * SIZE * 4) + x * 4 + 1] = 0u;                             // G channel
                            image_data[8 + (y * SIZE * 4) + x * 4 + 2] = ((x + y) % 2 == 0) ? 255u : 0u; // B channel
                            image_data[8 + (y * SIZE * 4) + x * 4 + 3] = 255u;                           // unused alpha channel
                        }
                    }
                    return std::make_shared<gc::RenderTexture>(render_backend.createTexture(image_data, true));
                }();
                m_fallback_material =
                    std::make_unique<gc::RenderMaterial>(render_backend.createMaterial(base_color_texture, orm_texture, normal_texture, pipeline));
            }
            m_fallback_material->waitForUpload();
            frame_state.draw_data.setFallbackMaterial(m_fallback_material.get());

            {
                auto skybox_pipeline = std::make_shared<gc::GPUPipeline>(render_backend.createSkyboxPipeline());
                std::array<std::span<const uint8_t>, 6> faces{};
                for (int i = 0; i < 6; ++i) {
                    faces[i] = content.findAsset(gc::Name(std::format("skybox{}.jpg", i)), gcpak::GcpakAssetType::TEXTURE_R8G8B8A8);
                }
                auto skybox_texture = std::make_shared<gc::RenderTexture>(render_backend.createCubeTexture(faces, true));
                m_skybox_material = std::make_unique<gc::RenderMaterial>(render_backend.createMaterial(skybox_texture, nullptr, nullptr, skybox_pipeline));
                frame_state.draw_data.setSkyboxMaterial(m_skybox_material.get());
                frame_state.draw_data.setSkyboxMaterial(nullptr);
            }

            m_meshes[0] = std::make_unique<gc::RenderMesh>(
                render_backend.createMeshFromAsset(content.findAsset(gc::Name("shrek.obj"), gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16)));
            m_meshes[1] = std::make_unique<gc::RenderMesh>(genSphereMesh(render_backend, 1.0f, 64));
            m_meshes[2] = std::make_unique<gc::RenderMesh>(genSphereMesh(render_backend, 0.5f, 16, true));
            m_meshes[3] = std::make_unique<gc::RenderMesh>(genCuboidMesh(render_backend, 100.0f, 100.0f, 1.0f, 25.0f));
            for (const auto& mesh : m_meshes) {
                mesh->waitForUpload();
            }

            world.registerComponent<SpinComponent, gc::ComponentArrayType::SPARSE>();
            world.registerComponent<MouseMoveComponent, gc::ComponentArrayType::SPARSE>();
            world.registerComponent<FollowComponent, gc::ComponentArrayType::SPARSE>();
            world.registerSystem<SpinSystem>();
            world.registerSystem<MouseMoveSystem>();
            world.registerSystem<FollowSystem>();

            {
                auto occlusion_roughness_metallic_texture = std::make_shared<gc::RenderTexture>(createORMTexture(render_backend, 0.5f, 0.0f));
                auto normal_texture = std::make_shared<gc::RenderTexture>(createNormalTexture(render_backend));
                for (int i = 0; i < s_texture_names.size(); ++i) {
                    const gc::Name texture_name = s_texture_names[i];
                    auto image_data = content.findAsset(texture_name, gcpak::GcpakAssetType::TEXTURE_R8G8B8A8);
                    if (!image_data.empty()) {
                        m_materials[i] = std::make_unique<gc::RenderMaterial>(
                            render_backend.createMaterial(std::make_shared<gc::RenderTexture>(render_backend.createTexture(image_data, true)),
                                                          occlusion_roughness_metallic_texture, normal_texture, pipeline));
                    }
                    else {
                        gc::abortGame("Couldn't find {}", texture_name.getString());
                    }
                }
            }
            {
                auto base_color_texture = std::make_shared<gc::RenderTexture>(
                    render_backend.createTexture(content.findAsset(gc::Name("bricks-mortar-albedo.png"), gcpak::GcpakAssetType::TEXTURE_R8G8B8A8), true));
                auto occlusion_roughness_metallic_texture = std::make_shared<gc::RenderTexture>(
                    render_backend.createTexture(content.findAsset(gc::Name("bricks-mortar-orm.png"), gcpak::GcpakAssetType::TEXTURE_R8G8B8A8), false));
                auto normal_texture = std::make_shared<gc::RenderTexture>(
                    render_backend.createTexture(content.findAsset(gc::Name("bricks-mortar-normal.png"), gcpak::GcpakAssetType::TEXTURE_R8G8B8A8), false));
                m_floor_material = std::make_unique<gc::RenderMaterial>(
                    render_backend.createMaterial(base_color_texture, occlusion_roughness_metallic_texture, normal_texture, pipeline));
            }

            std::array<gc::Entity, 36> cubes{};
            const gc::Entity parent = world.createEntity(gc::Name("parent"), gc::ENTITY_NONE, glm::vec3{0.0f, 15.0f, 5.5f});
            world.addComponent<SpinComponent>(parent).setAxis({0.3f, 0.4f, 1.0f}).setRadiansPerSecond(0.0f);

            for (int x = 0; x < 6; ++x) {
                for (int y = 0; y < 6; ++y) {
                    auto& cube = cubes[x * 6 + y];
                    cube = world.createEntity(gc::Name(std::format("cube{}.{}", x, y)), parent, glm::vec3{(x - 2.5f) * 2.0f, 0.0f, (y - 2.5f) * 2.0f});
                    world.addComponent<gc::CubeComponent>(cube).setMesh(m_meshes[1].get()).setMaterial(m_materials[x].get());
                    world.addComponent<SpinComponent>(cube).setAxis({1.0f, 0.0f, 0.7f}).setRadiansPerSecond(0.0f);
                }
            }

            // add a floor
            const auto floor = world.createEntity(gc::Name("floor"), gc::ENTITY_NONE, {0.0f, 0.0f, -0.5f});
            world.addComponent<gc::CubeComponent>(floor).setMesh(m_meshes[3].get()).setMaterial(m_floor_material.get());

            // camera
            auto camera = world.createEntity(gc::Name("light"), gc::ENTITY_NONE, {0.0f, 0.0f, 67.5f * 25.4e-3f});
            world.addComponent<gc::CameraComponent>(camera)
                .setFOV(glm::radians(45.0f))
                .setNearPlane(0.1f)
                .setFarPlane(1000.0f * 1000.0f * 100.0f)
                .setActive(true);
            world.addComponent<MouseMoveComponent>(camera).setMoveSpeed(3e8f).setAcceleration(40.0f).setDeceleration(100.0f).setSensitivity(1e-3f);
            world.addComponent<gc::CubeComponent>(camera).setVisible(false);

            const auto shrek_parent = world.createEntity(gc::Name("shrek_parent"), gc::ENTITY_NONE, glm::vec3{0.0f, +100.0f, 5.0f});
            const auto shrek = world.createEntity(gc::Name("shrek"), shrek_parent, glm::vec3{0.0f, +0.0f, -4.331f});
            world.addComponent<gc::CubeComponent>(shrek).setMaterial(m_materials[5].get()).setMesh(m_meshes[0].get());
            world.addComponent<FollowComponent>(shrek_parent).setTarget(camera).setMinDistance(5.0f).setSpeed(15.0f);

            // earth
            constexpr float equatorial_radius = 6'378.137 * 1000.0;
            constexpr float polar_radius = 6'356.752 * 1000.0;
            constexpr float earth_rotation_speed = 7.2722e-5f;
            const auto earth = world.createEntity(gc::Name("earth"), gc::ENTITY_NONE, {0.0f, 0.0f, -polar_radius * 1.5}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                                                  {equatorial_radius, equatorial_radius, polar_radius});
            world.addComponent<gc::CubeComponent>(earth).setMesh(m_meshes[1].get()).setMaterial(m_materials[6].get());
            world.addComponent<SpinComponent>(earth).setAxis({0.0f, 0.0f, 1.0f}).setRadiansPerSecond(earth_rotation_speed);

            // auto light_pivot = world.createEntity(gc::Name("light_pivot"), gc::ENTITY_NONE, {0.0f, 0.0f, 5.0f});
            // world.addComponent<SpinComponent>(light_pivot).setAxis({0.0f, 0.0f, 1.0f}).setRadiansPerSecond((2.0f));
            // const auto light = world.createEntity(gc::Name("light"), light_pivot, {0.0f, 10.0f, 0.0f});
            // world.addComponent<gc::CubeComponent>(light).setMesh(m_meshes[2].get()).setMaterial(m_materials[2].get());

            m_loaded = true;
        }
    }
};

const std::array<gc::Name, 7> WorldLoadSystem::s_texture_names{gc::Name("box.jpg"),  gc::Name("bricks.jpg"),  gc::Name("fire.jpg"),    gc::Name("nuke.jpg"),
                                                               gc::Name("moss.png"), gc::Name("uvcheck.png"), gc::Name("8k_earth.jpg")};

void buildAndStartGame(gc::App& app)
{
    // On Windows/NVIDIA, TRIPLE_BUFFERED gives horrible latency and TRIPLE_BUFFERED_UNTHROTTLED doesn't work properly so use double buffering instead
#ifdef WIN32
    app.renderBackend().setSyncMode(gc::RenderSyncMode::VSYNC_OFF);
#else
    app.renderBackend().setSyncMode(gc::RenderSyncMode::VSYNC_ON_TRIPLE_BUFFERED_UNTHROTTLED);
#endif

    app.window().setTitle("Hello world!");
    app.window().setIsResizable(true);
    app.window().setMouseCaptured(true);
    // win.setSize(0, 0, true);
    app.window().setWindowVisibility(true);

    // app.debugUI().active = true;

    gc::World& world = app.world();
    world.registerSystem<WorldLoadSystem>();
    app.run();
}
