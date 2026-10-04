//
// prefab_test.exe
//
// Creates a few prefabs by hand, saves them into a .gcpak file, and tests loading them into a World. See README.
//

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <array>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include <gcpak/gcpak.h>
#include <gcpak/gcpak_prefab.h>

#include <gamecore/gc_byte_reader.h>
#include <gamecore/gc_byte_writer.h>
#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_content.h>
#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_light_component.h>
#include <gamecore/gc_prefab.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_transform_system.h>
#include <gamecore/gc_world.h>

using namespace gc::literals;

// A component defined by the 'game' (this tool), to check that the engine can load components it has never heard of.
struct HealthComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("HealthComponent");

    int32_t health{100};
    int32_t max_health{100};
    float regen_per_second{0.0f};

    void serialise(gc::ByteWriter& writer) const
    {
        writer.writeU32(static_cast<uint32_t>(health));
        writer.writeU32(static_cast<uint32_t>(max_health));
        writer.writeF32(regen_per_second);
    }

    void deserialise(gc::ByteReader& reader)
    {
        health = static_cast<int32_t>(reader.readU32());
        max_health = static_cast<int32_t>(reader.readU32());
        regen_per_second = reader.readF32();
    }

    static constexpr size_t getSerialisedSize() { return 2 * sizeof(uint32_t) + sizeof(float); }
};

// Another one, which spawns a different prefab. Shows a component referring to an asset by name.
struct SpawnerComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("SpawnerComponent");

    gc::Name prefab{};
    float interval{1.0f};

    void serialise(gc::ByteWriter& writer) const
    {
        writer.writeU32(prefab.getHash());
        writer.writeF32(interval);
    }

    void deserialise(gc::ByteReader& reader)
    {
        prefab = gc::Name(reader.readU32());
        interval = reader.readF32();
    }

    static constexpr size_t getSerialisedSize() { return sizeof(uint32_t) + sizeof(float); }
};

// Has no serialisation functions, so it can't be stored in a prefab.
struct RuntimeOnlyComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("RuntimeOnlyComponent");

    gc::Entity target{gc::ENTITY_NONE};
};

// Serialisable, but never registered with the World. Loading a prefab that has one should skip it.
struct UnregisteredComponent {
    static constexpr auto NAME = gc::Name::createConstexpr("UnregisteredComponent");

    uint64_t value{};

    void serialise(gc::ByteWriter& writer) const { writer.writeU64(value); }
    void deserialise(gc::ByteReader& reader) { value = reader.readU64(); }
    static constexpr size_t getSerialisedSize() { return sizeof(uint64_t); }
};

static constexpr const char* PAK_FILE_NAME = "prefab_test.gcpak";
static constexpr const char* PREFAB_CRATE = "prefab_test/crate";
static constexpr const char* PREFAB_TURRET = "prefab_test/turret";
static constexpr const char* PREFAB_WORLD = "prefab_test/world";

static constexpr uint32_t CRATE_ENTITY_COUNT = 1;
static constexpr uint32_t TURRET_ENTITY_COUNT = 5;
static constexpr uint32_t WORLD_ENTITY_COUNT = 12;
static constexpr uint32_t WORLD_ROOT_COUNT = 7;

static int s_checks_run{};
static int s_checks_failed{};

template <typename... Args>
static void print(std::format_string<Args...> fmt, Args&&... args)
{
    const std::string line = std::format("[prefab_test] {}\n", std::format(fmt, std::forward<Args>(args)...));
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
}

static void check(bool condition, std::string_view what)
{
    ++s_checks_run;
    if (!condition) {
        ++s_checks_failed;
        print("    FAILED: {}", what);
    }
}

#define CHECK(expr) check((expr), #expr)

static void registerComponents(gc::World& world)
{
    world.registerComponent<gc::RenderableComponent, gc::ComponentArrayType::DENSE>();
    world.registerComponent<gc::CameraComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::LightComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<HealthComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<SpawnerComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<RuntimeOnlyComponent, gc::ComponentArrayType::SPARSE>();
}

static uint32_t countEntities(gc::World& world)
{
    uint32_t count{};
    world.forEach<gc::TransformComponent>([&](gc::Entity, gc::TransformComponent&) { ++count; });
    return count;
}

static uint32_t countRoots(gc::World& world)
{
    uint32_t count{};
    world.forEach<gc::TransformComponent>([&](gc::Entity, gc::TransformComponent& t) {
        if (t.getParent() == gc::ENTITY_NONE) {
            ++count;
        }
    });
    return count;
}

// returns ENTITY_NONE if there isn't one
static gc::Entity findChild(gc::World& world, gc::Entity parent, gc::Name name)
{
    for (const gc::Entity child : world.getSystem<gc::TransformSystem>().getChildren(parent)) {
        if (world.getComponent<gc::TransformComponent>(child)->name == name) {
            return child;
        }
    }
    return gc::ENTITY_NONE;
}

static bool nearlyEqual(const glm::vec3& a, const glm::vec3& b) { return glm::distance(a, b) < 1.0e-5f; }

//
// Creating the prefabs
//

static std::vector<uint8_t> toVector(std::span<const uint8_t> data) { return std::vector<uint8_t>(data.begin(), data.end()); }

// A single entity
static std::vector<uint8_t> makeCratePrefab()
{
    gc::PrefabWriter writer{};
    writer.beginEntity("crate"_name);
    writer.addComponent(gc::RenderableComponent{}.setMesh("cube"_name).setMaterial("bricks-mortar"_name));
    writer.addComponent(HealthComponent{.health = 25, .max_health = 25});
    return toVector(writer.getData());
}

// A hierarchy three levels deep, with engine and game components
static std::vector<uint8_t> makeTurretPrefab()
{
    gc::PrefabWriter writer{};

    const uint32_t base = writer.beginEntity("turret"_name, gcpak::PREFAB_NO_PARENT, glm::vec3{0.0f, 0.0f, 0.0f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                                             glm::vec3{2.0f, 2.0f, 0.5f});
    writer.addComponent(gc::RenderableComponent{}.setMesh("cube"_name).setMaterial("bricks-mortar"_name));
    writer.addComponent(HealthComponent{.health = 150, .max_health = 200, .regen_per_second = 2.5f});

    const uint32_t head = writer.beginEntity("head"_name, base, glm::vec3{0.0f, 0.0f, 1.5f}, glm::angleAxis(glm::radians(90.0f), glm::vec3{0.0f, 0.0f, 1.0f}));
    writer.addComponent(gc::RenderableComponent{}.setMesh("ball"_name).setMaterial("testmat"_name));
    writer.addComponent(SpawnerComponent{.prefab = gc::Name(PREFAB_CRATE), .interval = 0.5f});

    writer.beginEntity("barrel"_name, head, glm::vec3{0.0f, 1.0f, 0.0f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, glm::vec3{0.2f, 1.0f, 0.2f});
    writer.addComponent(gc::RenderableComponent{}.setMesh("cube"_name).setMaterial("testmat"_name));

    writer.beginEntity("muzzle_flash"_name, head, glm::vec3{0.0f, 2.0f, 0.0f});
    writer.addComponent(gc::LightComponent{});
    writer.addComponent(gc::RenderableComponent{}.setMesh("ball"_name).setVisible(false));

    // a second child of the base, declared after the head's children
    writer.beginEntity("status_light"_name, base, glm::vec3{0.9f, 0.9f, 0.5f});
    writer.addComponent(gc::LightComponent{});
    writer.addComponent(UnregisteredComponent{.value = 0x0123456789abcdefULL}); // the loader has to skip this

    return toVector(writer.getData());
}

// An entire game world: many root entities
static std::vector<uint8_t> makeWorldPrefab()
{
    gc::PrefabWriter writer{};

    writer.beginEntity("camera"_name, gcpak::PREFAB_NO_PARENT, glm::vec3{0.0f, -10.0f, 3.0f}, glm::angleAxis(glm::radians(80.0f), glm::vec3{1.0f, 0.0f, 0.0f}));
    writer.addComponent(gc::CameraComponent{}.setFOV(glm::radians(70.0f)).setNearPlane(0.25f));

    writer.beginEntity("sun"_name, gcpak::PREFAB_NO_PARENT, glm::vec3{0.0f, 0.0f, 50.0f}, glm::angleAxis(glm::radians(-45.0f), glm::vec3{1.0f, 0.0f, 0.0f}));
    writer.addComponent(gc::LightComponent{});

    writer.beginEntity("floor"_name, gcpak::PREFAB_NO_PARENT, glm::vec3{0.0f, 0.0f, -0.5f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, glm::vec3{40.0f, 40.0f, 1.0f});
    writer.addComponent(gc::RenderableComponent{}.setMesh("cube"_name).setMaterial("laminate-flooring-brown"_name));

    // an entity without any components other than its transform
    const uint32_t props = writer.beginEntity("props"_name);
    for (int i = 0; i < 3; ++i) {
        const std::string name = std::format("crate_{}", i);
        const uint32_t crate = writer.beginEntity(gc::Name(name), props, glm::vec3{static_cast<float>(i) * 3.0f - 3.0f, 4.0f, 0.5f});
        writer.addComponent(gc::RenderableComponent{}.setMesh("cube"_name).setMaterial("bricks-mortar"_name));
        writer.addComponent(HealthComponent{.health = 10 * (i + 1), .max_health = 50});
        if (i == 1) {
            writer.beginEntity("lid"_name, crate, glm::vec3{0.0f, 0.0f, 0.55f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, glm::vec3{1.1f, 1.1f, 0.1f});
            writer.addComponent(gc::RenderableComponent{}.setMesh("cube"_name).setMaterial("testmat"_name));
        }
    }

    for (int i = 0; i < 3; ++i) {
        const std::string name = std::format("spawner_{}", i);
        writer.beginEntity(gc::Name(name), gcpak::PREFAB_NO_PARENT, glm::vec3{-10.0f + 10.0f * static_cast<float>(i), 15.0f, 0.0f});
        writer.addComponent(SpawnerComponent{.prefab = gc::Name(PREFAB_TURRET), .interval = 5.0f + static_cast<float>(i)});
    }

    writer.beginEntity("player_start"_name, props, glm::vec3{0.0f, -2.0f, 0.0f});

    return toVector(writer.getData());
}

static bool writePakFile(const std::filesystem::path& directory)
{
    gcpak::GcpakCreator creator{};
    creator.addAsset({.name = PREFAB_CRATE, .hash = 0, .data = makeCratePrefab(), .type = gcpak::GcpakAssetType::PREFAB});
    creator.addAsset({.name = PREFAB_TURRET, .hash = 0, .data = makeTurretPrefab(), .type = gcpak::GcpakAssetType::PREFAB});
    creator.addAsset({.name = PREFAB_WORLD, .hash = 0, .data = makeWorldPrefab(), .type = gcpak::GcpakAssetType::PREFAB});
    // something that isn't a prefab, to check that loading it as one is refused
    creator.addAsset({.name = "prefab_test/not_a_prefab", .hash = 0, .data = {1, 2, 3, 4}, .type = gcpak::GcpakAssetType::SPIRV_SHADER});

    const std::filesystem::path path = directory / PAK_FILE_NAME;
    if (!creator.saveFile(path)) {
        print("Failed to write {}", path.string());
        return false;
    }
    for (const auto& asset : creator.getAssets()) {
        print("    {} ({} bytes)", asset.name, asset.data.size());
    }
    return true;
}

//
// Tests
//

static void testCrate(const gc::Content& content)
{
    print("Loading a single entity prefab");

    gc::World world{};
    registerComponents(world);

    std::vector<gc::Entity> entities{};
    const gc::Entity crate = gc::loadPrefab(content, gc::Name(PREFAB_CRATE), world, gc::ENTITY_NONE, &entities);
    CHECK(crate != gc::ENTITY_NONE);
    if (crate == gc::ENTITY_NONE) {
        return;
    }
    CHECK(entities.size() == CRATE_ENTITY_COUNT);
    CHECK(countEntities(world) == CRATE_ENTITY_COUNT);

    const auto* const transform = world.getComponent<gc::TransformComponent>(crate);
    CHECK(transform->name == "crate"_name);
    CHECK(transform->getParent() == gc::ENTITY_NONE);
    CHECK(nearlyEqual(transform->getScale(), glm::vec3{1.0f}));

    const auto* const renderable = world.getComponent<gc::RenderableComponent>(crate);
    CHECK(renderable != nullptr);
    if (renderable) {
        CHECK(renderable->m_visible);
        CHECK(renderable->m_mesh == "cube"_name);
        CHECK(renderable->m_material == "bricks-mortar"_name);
    }

    const auto* const health = world.getComponent<HealthComponent>(crate);
    CHECK(health != nullptr);
    if (health) {
        CHECK(health->health == 25);
        CHECK(health->max_health == 25);
    }

    CHECK(world.getComponent<gc::CameraComponent>(crate) == nullptr);
    CHECK(world.getComponent<SpawnerComponent>(crate) == nullptr);
}

static void testTurret(const gc::Content& content)
{
    print("Loading a hierarchy");

    gc::World world{};
    registerComponents(world);

    // Instantiate it twice under an existing entity, to check that parent indices are relative to the prefab.
    const gc::Entity parent = world.createEntity("turrets"_name, gc::ENTITY_NONE, glm::vec3{100.0f, 0.0f, 0.0f});
    std::vector<gc::Entity> entities{};
    const gc::Entity first = gc::loadPrefab(content, gc::Name(PREFAB_TURRET), world, parent, &entities);
    const gc::Entity second = gc::loadPrefab(content, gc::Name(PREFAB_TURRET), world, parent, &entities);
    CHECK(first != gc::ENTITY_NONE);
    CHECK(second != gc::ENTITY_NONE);
    CHECK(first != second);
    if (first == gc::ENTITY_NONE || second == gc::ENTITY_NONE) {
        return;
    }
    CHECK(entities.size() == 2 * TURRET_ENTITY_COUNT);
    CHECK(countEntities(world) == 1 + 2 * TURRET_ENTITY_COUNT);
    CHECK(world.getSystem<gc::TransformSystem>().getChildren(parent).size() == 2);

    for (const gc::Entity turret : {first, second}) {
        const auto* const transform = world.getComponent<gc::TransformComponent>(turret);
        CHECK(transform->name == "turret"_name);
        CHECK(transform->getParent() == parent);
        CHECK(nearlyEqual(transform->getScale(), glm::vec3{2.0f, 2.0f, 0.5f}));

        const auto* const health = world.getComponent<HealthComponent>(turret);
        CHECK(health != nullptr);
        if (health) {
            CHECK(health->health == 150);
            CHECK(health->max_health == 200);
            CHECK(health->regen_per_second == 2.5f);
        }

        CHECK(world.getSystem<gc::TransformSystem>().getChildren(turret).size() == 2);
        const gc::Entity head = findChild(world, turret, "head"_name);
        const gc::Entity status_light = findChild(world, turret, "status_light"_name);
        CHECK(head != gc::ENTITY_NONE);
        CHECK(status_light != gc::ENTITY_NONE);
        if (head == gc::ENTITY_NONE || status_light == gc::ENTITY_NONE) {
            continue;
        }

        const auto* const head_transform = world.getComponent<gc::TransformComponent>(head);
        CHECK(nearlyEqual(head_transform->getPosition(), glm::vec3{0.0f, 0.0f, 1.5f}));
        const glm::quat expected_rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3{0.0f, 0.0f, 1.0f});
        CHECK(glm::abs(glm::dot(head_transform->getRotation(), expected_rotation)) > 0.9999f);

        const auto* const spawner = world.getComponent<SpawnerComponent>(head);
        CHECK(spawner != nullptr);
        if (spawner) {
            CHECK(spawner->prefab == gc::Name(PREFAB_CRATE));
            CHECK(spawner->interval == 0.5f);
        }
        const auto* const head_renderable = world.getComponent<gc::RenderableComponent>(head);
        CHECK(head_renderable && head_renderable->m_mesh == "ball"_name && head_renderable->m_material == "testmat"_name);

        CHECK(world.getSystem<gc::TransformSystem>().getChildren(head).size() == 2);
        const gc::Entity barrel = findChild(world, head, "barrel"_name);
        const gc::Entity muzzle_flash = findChild(world, head, "muzzle_flash"_name);
        CHECK(barrel != gc::ENTITY_NONE);
        CHECK(muzzle_flash != gc::ENTITY_NONE);
        if (barrel != gc::ENTITY_NONE) {
            CHECK(nearlyEqual(world.getComponent<gc::TransformComponent>(barrel)->getScale(), glm::vec3{0.2f, 1.0f, 0.2f}));
            CHECK(world.getComponent<gc::RenderableComponent>(barrel) != nullptr);
            CHECK(world.getComponent<gc::LightComponent>(barrel) == nullptr);
        }
        if (muzzle_flash != gc::ENTITY_NONE) {
            CHECK(world.getComponent<gc::LightComponent>(muzzle_flash) != nullptr);
            const auto* const renderable = world.getComponent<gc::RenderableComponent>(muzzle_flash);
            CHECK(renderable && !renderable->m_visible && renderable->m_material.empty());
        }

        // the UnregisteredComponent was skipped, and what came before it in the entity was still loaded
        CHECK(world.getComponent<gc::LightComponent>(status_light) != nullptr);
        CHECK(world.getComponentList(status_light).size() == 2);
    }

    // The world matrices should take the whole hierarchy into account once the TransformSystem has run.
    gc::FrameState frame_state{};
    world.update(frame_state);
    const gc::Entity head = findChild(world, first, "head"_name);
    if (head != gc::ENTITY_NONE) {
        // parent at x=100, turret at the origin with z scale 0.5, head 1.5 up
        CHECK(nearlyEqual(world.getComponent<gc::TransformComponent>(head)->getWorldPosition(), glm::vec3{100.0f, 0.0f, 0.75f}));
    }

    // Deleting an instance removes all of it and leaves the other alone.
    world.deleteEntity(first);
    CHECK(countEntities(world) == 1 + TURRET_ENTITY_COUNT);
    CHECK(world.getSystem<gc::TransformSystem>().getChildren(second).size() == 2);
}

static void testWorld(const gc::Content& content)
{
    print("Loading an entire world");

    gc::World world{};
    registerComponents(world);

    std::vector<gc::Entity> entities{};
    const gc::Entity first_root = gc::loadPrefab(content, gc::Name(PREFAB_WORLD), world, gc::ENTITY_NONE, &entities);
    CHECK(first_root != gc::ENTITY_NONE);
    if (first_root == gc::ENTITY_NONE) {
        return;
    }
    CHECK(entities.size() == WORLD_ENTITY_COUNT);
    CHECK(countEntities(world) == WORLD_ENTITY_COUNT);
    CHECK(countRoots(world) == WORLD_ROOT_COUNT);
    CHECK(world.getComponent<gc::TransformComponent>(first_root)->name == "camera"_name);
    CHECK(world.getComponent<gc::CameraComponent>(first_root) != nullptr);

    const gc::Entity sun = world.findEntity("sun"_name);
    CHECK(sun != gc::ENTITY_NONE && world.getComponent<gc::LightComponent>(sun) != nullptr);

    const gc::Entity props = world.findEntity("props"_name);
    CHECK(props != gc::ENTITY_NONE);
    if (props != gc::ENTITY_NONE) {
        CHECK(world.getComponentList(props).size() == 1); // just the transform
        CHECK(world.getSystem<gc::TransformSystem>().getChildren(props).size() == 4);
        // declared last, long after its parent
        CHECK(findChild(world, props, "player_start"_name) != gc::ENTITY_NONE);

        const gc::Entity crate = findChild(world, props, "crate_1"_name);
        CHECK(crate != gc::ENTITY_NONE);
        if (crate != gc::ENTITY_NONE) {
            const auto* const health = world.getComponent<HealthComponent>(crate);
            CHECK(health && health->health == 20 && health->max_health == 50);
            CHECK(findChild(world, crate, "lid"_name) != gc::ENTITY_NONE);
        }
    }

    const gc::Entity spawner = world.findEntity("spawner_2"_name);
    CHECK(spawner != gc::ENTITY_NONE);
    if (spawner != gc::ENTITY_NONE) {
        const auto* const component = world.getComponent<SpawnerComponent>(spawner);
        CHECK(component && component->prefab == gc::Name(PREFAB_TURRET) && component->interval == 7.0f);

        // A component in the world can name a prefab, which is only loaded when it is wanted.
        if (component) {
            const gc::Entity turret = gc::loadPrefab(content, component->prefab, world, spawner);
            CHECK(turret != gc::ENTITY_NONE);
            CHECK(countEntities(world) == WORLD_ENTITY_COUNT + TURRET_ENTITY_COUNT);
            CHECK(countRoots(world) == WORLD_ROOT_COUNT);
        }
    }
}

static void testSaving(const gc::Content& content)
{
    print("Saving the world as a prefab and loading it into another world");

    gc::World world{};
    registerComponents(world);
    CHECK(gc::loadPrefab(content, gc::Name(PREFAB_WORLD), world) != gc::ENTITY_NONE);

    // Change the world at runtime, so that what is saved isn't just what was loaded.
    const gc::Entity props = world.findEntity("props"_name);
    const gc::Entity turret = gc::loadPrefab(content, gc::Name(PREFAB_TURRET), world, props);
    CHECK(turret != gc::ENTITY_NONE);
    if (props == gc::ENTITY_NONE || turret == gc::ENTITY_NONE) {
        return;
    }
    world.getComponent<gc::TransformComponent>(turret)->setPosition(5.0f, 6.0f, 7.0f);
    world.getComponent<HealthComponent>(turret)->health = 1;
    world.deleteEntity(world.findEntity("spawner_0"_name));
    const gc::Entity marker = world.createEntity("marker"_name, turret);
    world.addComponent<RuntimeOnlyComponent>(marker).target = turret; // can't be saved, the entity itself still is

    const uint32_t expected_entities = WORLD_ENTITY_COUNT + TURRET_ENTITY_COUNT - 1 + 1;
    CHECK(countEntities(world) == expected_entities);

    const std::vector<uint8_t> saved = gc::saveWorldAsPrefab(world);
    CHECK(!saved.empty());

    gc::World second_world{};
    registerComponents(second_world);
    std::vector<gc::Entity> entities{};
    CHECK(gc::loadPrefab(saved, second_world, gc::ENTITY_NONE, &entities) != gc::ENTITY_NONE);
    CHECK(entities.size() == expected_entities);
    CHECK(countEntities(second_world) == expected_entities);
    CHECK(countRoots(second_world) == WORLD_ROOT_COUNT - 1);
    CHECK(second_world.findEntity("spawner_0"_name) == gc::ENTITY_NONE);

    const gc::Entity second_props = second_world.findEntity("props"_name);
    const gc::Entity second_turret = findChild(second_world, second_props, "turret"_name);
    CHECK(second_turret != gc::ENTITY_NONE);
    if (second_turret != gc::ENTITY_NONE) {
        CHECK(nearlyEqual(second_world.getComponent<gc::TransformComponent>(second_turret)->getPosition(), glm::vec3{5.0f, 6.0f, 7.0f}));
        const auto* const health = second_world.getComponent<HealthComponent>(second_turret);
        CHECK(health && health->health == 1 && health->max_health == 200);
        const gc::Entity second_marker = findChild(second_world, second_turret, "marker"_name);
        CHECK(second_marker != gc::ENTITY_NONE);
        CHECK(second_marker != gc::ENTITY_NONE && second_world.getComponent<RuntimeOnlyComponent>(second_marker) == nullptr);
    }

    // Saving what was loaded must give exactly the same bytes.
    CHECK(gc::saveWorldAsPrefab(second_world) == saved);

    print("Saving part of a world as a prefab");
    const std::vector<uint8_t> saved_turret = gc::savePrefab(world, turret);
    gc::World third_world{};
    registerComponents(third_world);
    entities.clear();
    const gc::Entity third_turret = gc::loadPrefab(saved_turret, third_world, gc::ENTITY_NONE, &entities);
    CHECK(third_turret != gc::ENTITY_NONE);
    CHECK(entities.size() == TURRET_ENTITY_COUNT + 1);
    CHECK(countRoots(third_world) == 1);
    CHECK(third_turret != gc::ENTITY_NONE && gc::savePrefab(third_world, third_turret) == saved_turret);
}

// The engine can't know what the game's components are, so everything has to go through the World's by-name functions.
static void testComponentsByName()
{
    print("Serialising components by name");

    gc::World world{};
    registerComponents(world);

    CHECK(world.isComponentRegistered(gc::TransformComponent::NAME));
    CHECK(world.isComponentRegistered(HealthComponent::NAME));
    CHECK(world.isComponentRegistered(RuntimeOnlyComponent::NAME));
    CHECK(!world.isComponentRegistered(UnregisteredComponent::NAME));

    CHECK(world.getComponentSerialisedSize(HealthComponent::NAME) == HealthComponent::getSerialisedSize());
    CHECK(world.getComponentSerialisedSize(gc::LightComponent::NAME) == size_t{0});
    CHECK(!world.getComponentSerialisedSize(RuntimeOnlyComponent::NAME).has_value());
    CHECK(!world.getComponentSerialisedSize(UnregisteredComponent::NAME).has_value());

    const gc::Entity a = world.createEntity("a"_name);
    const gc::Entity b = world.createEntity("b"_name);
    world.addComponent<HealthComponent>(a) = HealthComponent{.health = 7, .max_health = 9, .regen_per_second = 0.125f};
    world.addComponent<RuntimeOnlyComponent>(a);

    std::array<uint8_t, 64> buffer{};
    gc::ByteWriter writer(buffer);
    CHECK(world.serialiseComponent(a, HealthComponent::NAME, writer));
    CHECK(writer.pos() == HealthComponent::getSerialisedSize());
    CHECK(!world.serialiseComponent(b, HealthComponent::NAME, writer)); // b doesn't have one
    CHECK(!world.serialiseComponent(a, RuntimeOnlyComponent::NAME, writer));
    CHECK(!world.serialiseComponent(a, UnregisteredComponent::NAME, writer));
    CHECK(writer.pos() == HealthComponent::getSerialisedSize());

    gc::ByteReader reader(std::span<const uint8_t>(buffer.data(), writer.pos()));
    CHECK(world.deserialiseComponent(b, HealthComponent::NAME, reader)); // adds the component
    const auto* const health = world.getComponent<HealthComponent>(b);
    CHECK(health && health->health == 7 && health->max_health == 9 && health->regen_per_second == 0.125f);
    CHECK(reader.remaining() == 0);

    reader.reset();
    world.getComponent<HealthComponent>(b)->health = 0;
    CHECK(world.deserialiseComponent(b, HealthComponent::NAME, reader)); // overwrites the existing one
    CHECK(world.getComponent<HealthComponent>(b)->health == 7);

    reader.reset();
    CHECK(!world.deserialiseComponent(b, RuntimeOnlyComponent::NAME, reader));
    CHECK(!world.deserialiseComponent(b, UnregisteredComponent::NAME, reader));
    CHECK(reader.pos() == 0);
}

// A corrupt prefab must be rejected without leaving anything behind in the world (and without crashing).
static void testCorruptData()
{
    print("Loading corrupt prefabs (errors are expected in the log)");

    gc::World world{};
    registerComponents(world);
    const gc::Entity parent = world.createEntity("parent"_name);

    const std::vector<uint8_t> turret = makeTurretPrefab();
    const auto loadFails = [&](std::span<const uint8_t> data) {
        const bool failed = gc::loadPrefab(data, world, parent) == gc::ENTITY_NONE;
        return failed && countEntities(world) == 1 && world.getSystem<gc::TransformSystem>().getChildren(parent).empty();
    };

    CHECK(loadFails({}));

    // Cut the prefab short at every possible length. It only loads if the cut happens to be between two declarations.
    uint32_t loaded{};
    uint32_t rejected{};
    bool clean{true};
    for (size_t length = 1; length < turret.size(); ++length) {
        std::vector<gc::Entity> entities{};
        const gc::Entity root = gc::loadPrefab(std::span<const uint8_t>(turret.data(), length), world, parent, &entities);
        if (root == gc::ENTITY_NONE) {
            ++rejected;
            clean = clean && entities.empty() && countEntities(world) == 1;
        }
        else {
            ++loaded;
            world.deleteEntity(root);
        }
    }
    CHECK(clean);
    CHECK(loaded == 13); // the turret is 14 declarations (5 entities and 9 components), so there are 13 places between them
    CHECK(rejected == turret.size() - 1 - 13);
    CHECK(countEntities(world) == 1);

    {
        // a component before any entity
        gc::PrefabWriter writer{};
        writer.beginEntity("removed"_name);
        const size_t first_entity_size = writer.getData().size();
        writer.addComponent(HealthComponent{});
        writer.beginEntity("late"_name);
        CHECK(loadFails(writer.getData().subspan(first_entity_size)));
    }
    {
        // a component that is the wrong size, after an entity that has to be removed again
        gc::PrefabWriter writer{};
        writer.beginEntity("root"_name);
        writer.beginEntity("child"_name, 0);
        writer.addComponentDeclaration(HealthComponent::NAME, HealthComponent::getSerialisedSize() - 1);
        CHECK(loadFails(writer.getData()));
    }
    {
        // parents that don't exist: the entity itself, and one far out of range
        for (const uint32_t bad_parent : {1u, 1000u, 0xfffffffeu}) {
            gc::PrefabWriter writer{};
            writer.beginEntity("root"_name);
            std::vector<uint8_t> data = toVector(writer.getData());
            const size_t second_entity = data.size();
            data.insert(data.end(), data.begin(), data.end()); // a second root...
            gc::ByteWriter parent_writer(std::span<uint8_t>(data).subspan(second_entity + gcpak::PREFAB_COMPONENT_HEADER_SIZE));
            parent_writer.writeU32(bad_parent); // ...given a bad parent
            CHECK(loadFails(data));
        }
    }
    {
        // an entity declaration of the wrong size
        gc::PrefabWriter writer{};
        writer.addComponentDeclaration(gc::TransformComponent::NAME, gc::TransformComponent::getSerialisedSize());
        CHECK(loadFails(writer.getData()));
    }
    {
        // a declaration that claims to be far bigger than the data
        gc::PrefabWriter writer{};
        writer.beginEntity("root"_name);
        std::vector<uint8_t> data = toVector(writer.getData());
        gc::ByteWriter size_writer(std::span<uint8_t>(data).subspan(sizeof(uint32_t)));
        size_writer.writeU32(0xffffffffu);
        CHECK(loadFails(data));
    }
}

static void testWrongAssets(const gc::Content& content)
{
    print("Loading assets that aren't prefabs (errors are expected in the log)");

    gc::World world{};
    registerComponents(world);
    CHECK(gc::loadPrefab(content, "prefab_test/not_a_prefab"_name, world) == gc::ENTITY_NONE);
    CHECK(gc::loadPrefab(content, "prefab_test/does_not_exist"_name, world) == gc::ENTITY_NONE);
    CHECK(countEntities(world) == 0);
}

// A world that hasn't registered some of the components still gets the hierarchy and the components that it does know.
static void testMissingComponents(const gc::Content& content)
{
    print("Loading into a world without most of the components (warnings are expected in the log)");

    // (components have to be registered in the same order in every World, so this can only leave out the later ones)
    gc::World world{};
    world.registerComponent<gc::RenderableComponent, gc::ComponentArrayType::DENSE>();

    const gc::Entity turret = gc::loadPrefab(content, gc::Name(PREFAB_TURRET), world);
    CHECK(turret != gc::ENTITY_NONE);
    CHECK(countEntities(world) == TURRET_ENTITY_COUNT);
    if (turret != gc::ENTITY_NONE) {
        CHECK(world.getComponent<gc::RenderableComponent>(turret) != nullptr);
        CHECK(world.getComponent<HealthComponent>(turret) == nullptr);
        CHECK(world.getComponentList(turret).size() == 2);
        CHECK(findChild(world, findChild(world, turret, "head"_name), "barrel"_name) != gc::ENTITY_NONE);
    }
}

int main(int argc, char* argv[])
{
    // The .gcpak file is left behind afterwards so that it can be looked at, or copied into a game's content directory.
    std::filesystem::path directory = std::filesystem::temp_directory_path() / "gamecore_prefab_test";
    if (argc >= 2) {
        directory = argv[1];
    }
    // Component names are only known to the engine by their hash. This makes the log readable in development builds.
    for (const char* const name : {"TransformComponent", "RenderableComponent", "CameraComponent", "LightComponent", "HealthComponent", "SpawnerComponent",
                                   "RuntimeOnlyComponent", "UnregisteredComponent"}) {
        (void)gc::Name(name);
    }

    std::error_code ec{};
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        print("Failed to create directory {}: {}", directory.string(), ec.message());
        return EXIT_FAILURE;
    }

    print("Writing prefabs to {}", (directory / PAK_FILE_NAME).string());
    if (!writePakFile(directory)) {
        return EXIT_FAILURE;
    }

    {
        // Only open the file that was just written, as the directory might have other .gcpak files in it.
        const std::array<std::string, 1> pak_files{PAK_FILE_NAME};
        const gc::Content content(directory, pak_files);

        testCrate(content);
        testTurret(content);
        testWorld(content);
        testSaving(content);
        testComponentsByName();
        testCorruptData();
        testWrongAssets(content);
        testMissingComponents(content);
    }

    if (s_checks_failed == 0) {
        print("PASSED ({} checks)", s_checks_run);
        return EXIT_SUCCESS;
    }
    else {
        print("FAILED ({} of {} checks)", s_checks_failed, s_checks_run);
        return EXIT_FAILURE;
    }
}
