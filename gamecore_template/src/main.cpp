#include <gamecore/gc_app.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_world.h>
#include <gamecore/gc_render_backend.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_cube_component.h>
#include <gamecore/gc_ecs.h>

#include <tracy/Tracy.hpp>

#include <SDL3/SDL_main.h>

class SpinComponent {
    friend class SpinSystem;
    float m_angle_radians{};
    glm::vec3 m_axis_norm{0.0f, 1.0f, 0.0f};
    float m_radians_per_second{1.0f};

public:
    SpinComponent& setRadiansPerSecond(float radians_per_second)
    {
        m_radians_per_second = radians_per_second;
        return *this;
    }
    SpinComponent& setAxis(const glm::vec3& axis)
    {
        m_axis_norm = glm::normalize(axis);
        return *this;
    }
};

class SpinSystem : public gc::System {

public:
    SpinSystem(gc::World& world) : gc::System(world) {}

    void onUpdate(gc::FrameState& frame_state) override
    {
        ZoneScoped;
        m_world.forEach<gc::TransformComponent, SpinComponent>([&]([[maybe_unused]] gc::Entity entity, gc::TransformComponent& t, SpinComponent& s) {
            t.setRotation(glm::angleAxis(s.m_angle_radians, s.m_axis_norm));
            s.m_angle_radians += static_cast<float>(frame_state.delta_time) * s.m_radians_per_second;
        });
    }
};

class MouseMoveComponent {
public:
    float sensitivity{1.0f};
};

class MouseMoveSystem : public gc::System {

public:
    MouseMoveSystem(gc::World& world) : gc::System(world) {}

    void onUpdate(gc::FrameState& frame_state) override
    {
        ZoneScoped;
        m_world.forEach<gc::TransformComponent, MouseMoveComponent>([&]([[maybe_unused]] gc::Entity entity, gc::TransformComponent& t, MouseMoveComponent& m) {
            // change x and y positions by mouse delta
            t.setPosition(t.getPosition() + glm::vec3{frame_state.window_state->getMouseMotion() * m.sensitivity, 0.0f});
        });
    }
};

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{

    gc::AppInitOptions init_options{};
    init_options.name = "gamecore_template";
    init_options.author = "bailwillharr";
    init_options.version = "v0.0.0";

    gc::App::initialise(init_options);

    gc::App& app = gc::app();

    // On Windows/NVIDIA, triple buffered gives horrible latency so use double buffering instead
    app.renderBackend().setSyncMode(gc::RenderSyncMode::VSYNC_ON_DOUBLE_BUFFERED);

    gc::Window& win = app.window();
    win.setTitle("Hello world!");
    win.setIsResizable(true);
    win.setMouseCaptured(true);
    win.setSize(0, 0, true);

    gc::World& world = app.world();

    world.registerComponent<SpinComponent, gc::ComponentArrayType::DENSE>();
    world.registerComponent<MouseMoveComponent, gc::ComponentArrayType::SPARSE>();
    world.registerSystem<SpinSystem>();
    world.registerSystem<MouseMoveSystem>();

    std::array<gc::Entity, 36> cubes{};
    const gc::Entity parent = world.createEntity(gc::strToName("parent"), gc::ENTITY_NONE, glm::vec3{0.0f, 0.0f, 25.0f});
    world.addComponent<SpinComponent>(parent);
    world.addComponent<MouseMoveComponent>(parent).sensitivity = 0.01f;
    for (int x = 0; x < 6; ++x) {
        for (int y = 0; y < 6; ++y) {
            auto& cube = cubes[x * 6 + y];
            cube = world.createEntity(gc::strToNameRuntime(std::format("cube{}.{}", x, y)), parent, glm::vec3{x * 3.0f - 9.0f, y * 3.0f - 9.0f, 0.0f});
            world.addComponent<gc::CubeComponent>(cube);
            world.addComponent<SpinComponent>(cube).setAxis({1.0f, 0.0f, 0.0f}).setRadiansPerSecond(-2.0f);
        }
    }

    world.deleteEntity(cubes[10]);

    const auto another_entity = world.createEntity(gc::strToName("ANOTHER ENTITY"), gc::ENTITY_NONE, {0.0f, 0.0f, 10.0f});
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "IMPORTANT MESSAGE!", std::format("Entity ID: {}", static_cast<uint32_t>(another_entity)).c_str(), nullptr);
    world.addComponent<gc::CubeComponent>(another_entity).visible = true;

    win.setWindowVisibility(true);

    app.run();

    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame() therefore main() can always return 0
    return 0;
}
