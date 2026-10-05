//
// collision_test.exe
//
// Tests the CollisionSystem against slow but obviously correct versions of its queries. See README.
//

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <limits>
#include <random>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <gamecore/gc_collider_component.h>
#include <gamecore/gc_collision_system.h>
#include <gamecore/gc_content.h>
#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_gen_mesh.h>
#include <gamecore/gc_resource_manager.h>
#include <gamecore/gc_resources.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_world.h>

using namespace gc::literals;

static int s_checks = 0;
static int s_failures = 0;

template <typename... Args>
static void print(std::format_string<Args...> fmt, Args&&... args)
{
    const std::string line = std::format("[collision_test] {}\n", std::format(fmt, std::forward<Args>(args)...));
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
}

static void check(bool condition, const char* what)
{
    ++s_checks;
    if (!condition) {
        ++s_failures;
        print("    FAILED: {}", what);
    }
}

#define CHECK(expr) check((expr), #expr)

//
// The slow versions: every triangle of every collider, in world space, in double precision
//

struct Triangle {
    glm::dvec3 a, b, c;
    gc::Entity entity;
};

static std::vector<Triangle> getWorldTriangles(gc::World& world, gc::ResourceManager& resource_manager)
{
    std::vector<Triangle> triangles{};
    world.forEach<gc::TransformComponent, gc::ColliderComponent>([&](gc::Entity entity, const gc::TransformComponent& t, const gc::ColliderComponent& c) {
        const gc::ResourceMesh* const mesh = resource_manager.get<gc::ResourceMesh>(c.m_mesh);
        if (!mesh) {
            return;
        }
        const glm::dmat4 matrix{t.getWorldMatrix()};
        const auto vertices = mesh->vertices.get();
        const auto indices = mesh->indices.get();
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            Triangle triangle{};
            triangle.a = glm::dvec3(matrix * glm::dvec4(glm::dvec3(vertices[indices[i]].position), 1.0));
            triangle.b = glm::dvec3(matrix * glm::dvec4(glm::dvec3(vertices[indices[i + 1]].position), 1.0));
            triangle.c = glm::dvec3(matrix * glm::dvec4(glm::dvec3(vertices[indices[i + 2]].position), 1.0));
            triangle.entity = entity;
            triangles.push_back(triangle);
        }
    });
    return triangles;
}

// returns the distance, or a negative number if the ray misses
static double slowRayTriangle(const glm::dvec3& origin, const glm::dvec3& direction, const Triangle& triangle)
{
    const glm::dvec3 edge1 = triangle.b - triangle.a;
    const glm::dvec3 edge2 = triangle.c - triangle.a;
    const glm::dvec3 p = glm::cross(direction, edge2);
    const double determinant = glm::dot(edge1, p);
    if (determinant == 0.0) {
        return -1.0;
    }
    const glm::dvec3 to_origin = origin - triangle.a;
    const double u = glm::dot(to_origin, p) / determinant;
    const glm::dvec3 q = glm::cross(to_origin, edge1);
    const double v = glm::dot(direction, q) / determinant;
    if (u < 0.0 || v < 0.0 || u + v > 1.0) {
        return -1.0;
    }
    return glm::dot(edge2, q) / determinant;
}

static glm::dvec3 slowClosestPoint(const glm::dvec3& p, const Triangle& triangle)
{
    // the closest point is on the triangle's plane if that is inside the triangle, otherwise on one of its edges
    const glm::dvec3 normal = glm::cross(triangle.b - triangle.a, triangle.c - triangle.a);
    const double normal_squared = glm::dot(normal, normal);
    if (normal_squared > 0.0) {
        const glm::dvec3 on_plane = p - normal * (glm::dot(p - triangle.a, normal) / normal_squared);
        const bool inside = glm::dot(glm::cross(triangle.b - triangle.a, on_plane - triangle.a), normal) >= 0.0 &&
                            glm::dot(glm::cross(triangle.c - triangle.b, on_plane - triangle.b), normal) >= 0.0 &&
                            glm::dot(glm::cross(triangle.a - triangle.c, on_plane - triangle.c), normal) >= 0.0;
        if (inside) {
            return on_plane;
        }
    }
    glm::dvec3 best = triangle.a;
    double best_distance = std::numeric_limits<double>::max();
    const std::array<std::pair<glm::dvec3, glm::dvec3>, 3> edges{{{triangle.a, triangle.b}, {triangle.b, triangle.c}, {triangle.c, triangle.a}}};
    for (const auto& [start, end] : edges) {
        const glm::dvec3 edge = end - start;
        const double length_squared = glm::dot(edge, edge);
        const double along = (length_squared > 0.0) ? std::clamp(glm::dot(p - start, edge) / length_squared, 0.0, 1.0) : 0.0;
        const glm::dvec3 point = start + edge * along;
        const double distance = glm::distance(point, p);
        if (distance < best_distance) {
            best_distance = distance;
            best = point;
        }
    }
    return best;
}

//
// Comparisons
//

static std::mt19937 s_random{12345};

static float randomFloat(float min, float max) { return std::uniform_real_distribution<float>(min, max)(s_random); }

static glm::vec3 randomVec3(float min, float max) { return glm::vec3{randomFloat(min, max), randomFloat(min, max), randomFloat(min, max)}; }

static void compareRaycasts(gc::World& world, gc::ResourceManager& resource_manager, const gc::CollisionSystem& collision, int ray_count, const char* what)
{
    const std::vector<Triangle> triangles = getWorldTriangles(world, resource_manager);

    int hits = 0;
    int wrong = 0;
    for (int i = 0; i < ray_count; ++i) {
        gc::Ray ray{};
        ray.origin = randomVec3(-25.0f, 25.0f);
        ray.direction = randomVec3(-1.0f, 1.0f);
        if (glm::length(ray.direction) < 0.1f) {
            continue;
        }
        const glm::dvec3 direction = glm::normalize(glm::dvec3(ray.direction));

        double expected = std::numeric_limits<double>::max();
        gc::Entity expected_entity = gc::ENTITY_NONE;
        for (const Triangle& triangle : triangles) {
            const double t = slowRayTriangle(glm::dvec3(ray.origin), direction, triangle);
            if (t >= 0.0 && t < expected) {
                expected = t;
                expected_entity = triangle.entity;
            }
        }

        const gc::RaycastHit hit = collision.raycast(ray);
        const bool expected_hit = (expected_entity != gc::ENTITY_NONE);
        hits += hit.hit ? 1 : 0;
        if (hit.hit != expected_hit) {
            ++wrong;
        }
        else if (hit.hit) {
            // The entity can legitimately differ where two colliders touch, so only the distance is compared
            const bool distance_ok = std::abs(static_cast<double>(hit.distance) - expected) < 1.0e-2;
            const bool position_ok = glm::distance(glm::dvec3(hit.position), glm::dvec3(ray.origin) + direction * expected) < 1.0e-2;
            const bool normal_ok = std::abs(glm::length(hit.normal) - 1.0f) < 1.0e-3f && glm::dot(hit.normal, glm::vec3(direction)) <= 1.0e-4f;
            if (!distance_ok || !position_ok || !normal_ok) {
                ++wrong;
            }
        }
    }
    print("    {}: {} rays, {} hit, {} differ from the slow version", what, ray_count, hits, wrong);
    // A ray that grazes the very edge of a mesh can go either way, as the system works in single precision
    CHECK(wrong <= ray_count / 500);
    CHECK(hits > ray_count / 20);
}

static void compareSpheres(gc::World& world, gc::ResourceManager& resource_manager, const gc::CollisionSystem& collision, int sphere_count, const char* what)
{
    const std::vector<Triangle> triangles = getWorldTriangles(world, resource_manager);

    int contact_count = 0;
    int wrong = 0;
    for (int i = 0; i < sphere_count; ++i) {
        const glm::vec3 centre = randomVec3(-20.0f, 20.0f);
        const float radius = randomFloat(0.2f, 4.0f);

        // the nearest point of each collider
        std::unordered_map<gc::Entity, double> expected{};
        for (const Triangle& triangle : triangles) {
            const double distance = glm::distance(slowClosestPoint(glm::dvec3(centre), triangle), glm::dvec3(centre));
            if (distance < static_cast<double>(radius)) {
                const auto it = expected.try_emplace(triangle.entity, distance).first;
                it->second = std::min(it->second, distance);
            }
        }

        std::vector<gc::SphereContact> contacts{};
        collision.overlapSphere(centre, radius, contacts);
        contact_count += static_cast<int>(contacts.size());

        bool ok = true;
        for (const gc::SphereContact& contact : contacts) {
            const auto it = expected.find(contact.entity);
            if (it == expected.end()) {
                // touching by less than the precision of the system
                ok = ok && contact.depth < 1.0e-3f;
                continue;
            }
            ok = ok && std::abs(static_cast<double>(radius - contact.depth) - it->second) < 1.0e-2;
            ok = ok && std::abs(glm::length(contact.normal) - 1.0f) < 1.0e-3f;
            // the contact point is where the normal and depth say it is
            ok = ok && glm::distance(contact.position + contact.normal * (radius - contact.depth), centre) < 1.0e-2f;
            expected.erase(it);
        }
        for (const auto& [entity, distance] : expected) {
            ok = ok && (static_cast<double>(radius) - distance) < 1.0e-3; // missed, but barely touching
        }
        wrong += ok ? 0 : 1;
    }
    print("    {}: {} spheres, {} contacts, {} differ from the slow version", what, sphere_count, contact_count, wrong);
    CHECK(wrong == 0);
    CHECK(contact_count > sphere_count / 20);
}

static void compareBoxQueries(gc::World& world, const gc::CollisionSystem& collision, int box_count)
{
    int wrong = 0;
    for (int i = 0; i < box_count; ++i) {
        gc::AABB box{};
        box.min = randomVec3(-20.0f, 15.0f);
        box.max = box.min + randomVec3(0.5f, 8.0f);

        std::vector<gc::Entity> expected{};
        world.forEach<gc::ColliderComponent>([&](gc::Entity entity, const gc::ColliderComponent&) {
            if (const auto collider_box = collision.getColliderAABB(entity); collider_box) {
                if (glm::all(glm::lessThanEqual(collider_box->min, box.max)) && glm::all(glm::greaterThanEqual(collider_box->max, box.min))) {
                    expected.push_back(entity);
                }
            }
        });
        std::vector<gc::Entity> found{};
        collision.queryAABB(box, found);
        std::sort(expected.begin(), expected.end());
        std::sort(found.begin(), found.end());
        wrong += (expected == found) ? 0 : 1;
    }
    print("    {} box queries, {} differ from the slow version", box_count, wrong);
    CHECK(wrong == 0);
}

int main()
{
    for (const char* const name : {"TransformComponent", "ColliderComponent", "cube", "ball", "plane", "missing"}) {
        (void)gc::Name(name);
    }

    // The engine needs a content directory, even though every resource here is made in code
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "gamecore_collision_test";
    std::error_code ec{};
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        print("Failed to create directory {}: {}", directory.string(), ec.message());
        return EXIT_FAILURE;
    }
    const gc::Content content(directory, {});
    gc::ResourceManager resource_manager(content);
    resource_manager.add<gc::ResourceMesh>(gc::genCubeMesh(), "cube"_name);
    resource_manager.add<gc::ResourceMesh>(gc::genSphereMesh(12), "ball"_name);
    resource_manager.add<gc::ResourceMesh>(gc::genPlaneMesh(), "plane"_name);

    gc::World world{};
    world.registerComponent<gc::ColliderComponent, gc::ComponentArrayType::SPARSE>();
    world.registerSystem<gc::CollisionSystem>(resource_manager);
    gc::CollisionSystem& collision = world.getSystem<gc::CollisionSystem>();
    gc::FrameState frame_state{};

    print("An empty world");
    world.update(frame_state);
    CHECK(collision.getColliderCount() == 0);
    CHECK(!collision.raycast(gc::Ray{glm::vec3{0.0f}, glm::vec3{1.0f, 0.0f, 0.0f}}).hit);

    print("One cube");
    const gc::Entity first = world.createEntity("cube"_name, gc::ENTITY_NONE, glm::vec3{10.0f, 0.0f, 0.0f});
    world.addComponent<gc::ColliderComponent>(first).setMesh("cube"_name);
    world.update(frame_state);
    CHECK(collision.getColliderCount() == 1);
    CHECK(collision.getMeshCount() == 1);
    {
        const auto box = collision.getColliderAABB(first);
        CHECK(box.has_value());
        const gc::RaycastHit hit = collision.raycast(gc::Ray{glm::vec3{0.0f, 0.0f, 0.0f}, glm::vec3{2.0f, 0.0f, 0.0f}});
        CHECK(hit.hit && hit.entity == first);
        // the ray stops at the cube's nearest face, which faces back along the ray
        CHECK(box && std::abs(hit.distance - box->min.x) < 1.0e-4f);
        CHECK(glm::distance(hit.normal, glm::vec3{-1.0f, 0.0f, 0.0f}) < 1.0e-4f);
        CHECK(!collision.raycast(gc::Ray{glm::vec3{0.0f}, glm::vec3{1.0f, 0.0f, 0.0f}}, 1.0f).hit);        // too short
        CHECK(!collision.raycast(gc::Ray{glm::vec3{0.0f}, glm::vec3{-1.0f, 0.0f, 0.0f}}).hit);             // wrong way
        CHECK(!collision.raycast(gc::Ray{glm::vec3{0.0f}, glm::vec3{1.0f, 0.0f, 0.0f}}, 100.0f, first).hit); // ignored
    }

    print("Many colliders, with every kind of transform");
    std::vector<gc::Entity> entities{first};
    const gc::Entity parent = world.createEntity("parent"_name, gc::ENTITY_NONE, glm::vec3{0.0f, 0.0f, 2.0f},
                                                 glm::angleAxis(0.7f, glm::normalize(glm::vec3{0.3f, 1.0f, 0.2f})), glm::vec3{1.5f, 1.5f, 1.5f});
    for (int i = 0; i < 300; ++i) {
        const glm::quat rotation = glm::angleAxis(randomFloat(0.0f, 6.28f), glm::normalize(randomVec3(-1.0f, 1.0f) + glm::vec3{0.0f, 0.0f, 1.5f}));
        const glm::vec3 scale = (i % 3 == 0) ? randomVec3(0.5f, 4.0f) : glm::vec3{randomFloat(0.5f, 3.0f)}; // some squashed
        const gc::Entity entity =
            world.createEntity("collider"_name, (i % 4 == 0) ? parent : gc::ENTITY_NONE, randomVec3(-18.0f, 18.0f), rotation, scale);
        world.addComponent<gc::ColliderComponent>(entity).setMesh((i % 2 == 0) ? "cube"_name : ((i % 5 == 0) ? "plane"_name : "ball"_name));
        entities.push_back(entity);
    }
    world.update(frame_state); // the TransformSystem works out the world matrices, then the CollisionSystem reads them
    CHECK(collision.getColliderCount() == entities.size());
    CHECK(collision.getMeshCount() == 3);
    print("    the broad phase tree is {} deep for {} colliders", collision.getTreeHeight(), collision.getColliderCount());
    CHECK(collision.getTreeHeight() < 40);
    compareRaycasts(world, resource_manager, collision, 600, "ray casts");
    compareSpheres(world, resource_manager, collision, 300, "spheres");
    compareBoxQueries(world, collision, 300);

    print("Moving colliders");
    for (size_t i = 0; i < entities.size(); i += 3) {
        world.getComponent<gc::TransformComponent>(entities[i])->setPosition(randomVec3(-18.0f, 18.0f));
    }
    world.getComponent<gc::TransformComponent>(parent)->setPosition(-3.0f, 4.0f, 0.0f); // moves its children too
    world.update(frame_state);
    compareRaycasts(world, resource_manager, collision, 600, "ray casts after moving");
    compareSpheres(world, resource_manager, collision, 300, "spheres after moving");
    compareBoxQueries(world, collision, 300);

    print("Changing meshes");
    for (size_t i = 1; i < entities.size(); i += 2) {
        gc::ColliderComponent& collider = *world.getComponent<gc::ColliderComponent>(entities[i]);
        collider.setMesh((collider.m_mesh == "cube"_name) ? "ball"_name : "cube"_name);
    }
    world.update(frame_state);
    compareRaycasts(world, resource_manager, collision, 600, "ray casts after changing meshes");
    compareSpheres(world, resource_manager, collision, 300, "spheres after changing meshes");

    print("A mesh that doesn't exist, and no mesh (an error is logged for the first)");
    world.getComponent<gc::ColliderComponent>(entities[1])->setMesh("missing"_name);
    world.getComponent<gc::ColliderComponent>(entities[2])->setMesh({});
    world.update(frame_state);
    CHECK(collision.getColliderCount() == entities.size());
    CHECK(!collision.getColliderAABB(entities[1]).has_value());
    CHECK(!collision.getColliderAABB(entities[2]).has_value());
    compareRaycasts(world, resource_manager, collision, 400, "ray casts with missing meshes");

    print("Removing colliders: deleted entities and removed components");
    size_t remaining = entities.size();
    for (size_t i = 0; i < entities.size(); ++i) {
        if (entities[i] == first) {
            continue;
        }
        if (i % 4 == 1) {
            world.deleteEntity(entities[i]);
            --remaining;
        }
        else if (i % 4 == 2) {
            world.removeComponent<gc::ColliderComponent>(entities[i]);
            --remaining;
        }
    }
    world.update(frame_state);
    CHECK(collision.getColliderCount() == remaining);
    compareRaycasts(world, resource_manager, collision, 600, "ray casts after removing");
    compareSpheres(world, resource_manager, collision, 300, "spheres after removing");
    compareBoxQueries(world, collision, 300);

    print("Many frames of things moving, appearing and disappearing");
    for (int frame = 0; frame < 200; ++frame) {
        world.forEach<gc::TransformComponent, gc::ColliderComponent>([&](gc::Entity entity, gc::TransformComponent& t, const gc::ColliderComponent&) {
            if ((entity + static_cast<gc::Entity>(frame)) % 7 == 0) {
                t.setPosition(t.getPosition() + randomVec3(-0.5f, 0.5f));
            }
        });
        if (frame % 3 == 0) {
            const gc::Entity entity = world.createEntity("extra"_name, gc::ENTITY_NONE, randomVec3(-18.0f, 18.0f));
            world.addComponent<gc::ColliderComponent>(entity).setMesh((frame % 2 == 0) ? "cube"_name : "ball"_name);
        }
        if (frame % 5 == 0) {
            gc::Entity victim = gc::ENTITY_NONE;
            world.forEach<gc::ColliderComponent>([&](gc::Entity entity, const gc::ColliderComponent&) {
                if (entity != first && (victim == gc::ENTITY_NONE || entity % 11 == static_cast<gc::Entity>(frame) % 11)) {
                    victim = entity;
                }
            });
            if (victim != gc::ENTITY_NONE) {
                world.deleteEntity(victim);
            }
        }
        world.update(frame_state);
    }
    print("    the broad phase tree is {} deep for {} colliders", collision.getTreeHeight(), collision.getColliderCount());
    compareRaycasts(world, resource_manager, collision, 600, "ray casts after many frames");
    compareSpheres(world, resource_manager, collision, 300, "spheres after many frames");
    compareBoxQueries(world, collision, 300);

    print("Removing everything");
    std::vector<gc::Entity> all{};
    world.forEach<gc::ColliderComponent>([&](gc::Entity entity, const gc::ColliderComponent&) { all.push_back(entity); });
    for (const gc::Entity entity : all) {
        world.removeComponent<gc::ColliderComponent>(entity);
    }
    world.update(frame_state);
    CHECK(collision.getColliderCount() == 0);
    CHECK(collision.getMeshCount() == 0);
    CHECK(collision.getTreeHeight() == 0);
    CHECK(!collision.raycast(gc::Ray{glm::vec3{0.0f}, glm::vec3{1.0f, 0.0f, 0.0f}}).hit);

    if (s_failures == 0) {
        print("PASSED ({} checks)", s_checks);
        return EXIT_SUCCESS;
    }
    print("FAILED ({} of {} checks)", s_failures, s_checks);
    return EXIT_FAILURE;
}
