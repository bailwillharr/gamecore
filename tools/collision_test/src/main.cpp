//
// collision_test.exe
//
// Tests the CollisionSystem against slow but obviously correct versions of its queries. See README.
//

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
#include <gamecore/gc_collision_bvh.h>
#include <gamecore/gc_collision_bvh_component.h>
#include <gamecore/gc_collision_system.h>
#include <gamecore/gc_content.h>
#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_gen_mesh.h>
#include <gamecore/gc_resource_manager.h>
#include <gamecore/gc_resources.h>
#include <gamecore/gc_terrain.h>
#include <gamecore/gc_terrain_builder.h>
#include <gamecore/gc_terrain_component.h>
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

//
// Terrains: the surface is the most detailed tile at each point. The slow versions go through every cell of every tile that is
// part of that surface, found straight from the tree.
//

static void appendTerrainTriangles(const gc::ResourceTerrain& terrain, const gc::TerrainNodeRef& node, const glm::dmat4& matrix, gc::Entity entity,
                                   std::vector<Triangle>& triangles)
{
    const uint32_t quads = terrain.header.tile_quads;
    const uint32_t side = quads + 1;
    const double cell = static_cast<double>(terrain.header.sample_spacing) * static_cast<double>(1u << node.level);
    const double node_x = static_cast<double>(node.x) * quads * cell;
    const double node_y = static_cast<double>(node.y) * quads * cell;
    const auto tile = terrain.getTile(node.index);
    const auto corner = [&](uint32_t i, uint32_t j) {
        const double height = static_cast<double>(terrain.header.height_offset) +
                              static_cast<double>(tile[j * side + i] & 0xffff) * static_cast<double>(terrain.header.height_scale);
        return glm::dvec3(matrix * glm::dvec4(node_x + i * cell, node_y + j * cell, height, 1.0));
    };
    for (uint32_t quarter = 0; quarter < 4; ++quarter) {
        const uint32_t child = (node.level > 0) ? terrain.nodes[node.index].children[quarter] : gc::TERRAIN_NO_NODE;
        if (child != gc::TERRAIN_NO_NODE) {
            appendTerrainTriangles(terrain, terrain.getChild(node, quarter), matrix, entity, triangles);
            continue;
        }
        const uint32_t x0 = (quarter & 1) ? quads / 2 : 0;
        const uint32_t y0 = (quarter & 2) ? quads / 2 : 0;
        for (uint32_t j = y0; j < y0 + quads / 2; ++j) {
            for (uint32_t i = x0; i < x0 + quads / 2; ++i) {
                triangles.push_back(Triangle{corner(i, j), corner(i + 1, j), corner(i + 1, j + 1), entity});
                triangles.push_back(Triangle{corner(i, j), corner(i + 1, j + 1), corner(i, j + 1), entity});
            }
        }
    }
}

static std::vector<Triangle> getTerrainTriangles(const gc::ResourceTerrain& terrain, const glm::mat4& matrix, gc::Entity entity)
{
    std::vector<Triangle> triangles{};
    for (uint32_t y = 0; y < terrain.header.root_count_y; ++y) {
        for (uint32_t x = 0; x < terrain.header.root_count_x; ++x) {
            appendTerrainTriangles(terrain, terrain.getRoot(x, y), glm::dmat4(matrix), entity, triangles);
        }
    }
    return triangles;
}

static void testTerrains(gc::ResourceManager& resource_manager)
{
    print("Terrains: making one");
    // Hills, with a hole in the data and a corner missing, like the edges of a scan
    constexpr uint32_t WIDTH = 301;
    constexpr uint32_t HEIGHT = 213;
    constexpr float SPACING = 0.75f;
    std::vector<float> heights(static_cast<size_t>(WIDTH) * HEIGHT);
    uint32_t data_count = 0;
    for (uint32_t y = 0; y < HEIGHT; ++y) {
        for (uint32_t x = 0; x < WIDTH; ++x) {
            const float fx = static_cast<float>(x);
            const float fy = static_cast<float>(y);
            float h = 20.0f * std::sin(fx * 0.05f) * std::cos(fy * 0.07f) + 3.0f * std::sin(fx * 0.31f + fy * 0.17f) + 100.0f;
            if ((x % 37 == 0 && y % 11 < 3)) {
                h += 6.0f; // cliffs
            }
            const bool hole = (fx - 120.0f) * (fx - 120.0f) + (fy - 90.0f) * (fy - 90.0f) < 30.0f * 30.0f;
            const bool corner = x > 230 && y > 160;
            heights[static_cast<size_t>(y) * WIDTH + x] = (hole || corner) ? std::numeric_limits<float>::quiet_NaN() : h;
            data_count += (hole || corner) ? 0 : 1;
        }
    }
    gc::TerrainBuildInput input{};
    input.width = WIDTH;
    input.height = HEIGHT;
    input.heights = heights;
    input.sample_spacing = SPACING;
    input.tile_quads = 16;
    input.fill_level = 2;
    gc::TerrainBuildInfo info{};
    std::string error{};
    // (kept until the end, as the resource is a view of it)
    static std::vector<uint8_t> s_data{};
    s_data = gc::makeTerrainData(input, &info, &error);
    CHECK(!s_data.empty());
    if (s_data.empty()) {
        print("    {}", error);
        return;
    }
    print("    {} levels, {} x {} top level tiles, {} tiles", info.level_count, info.root_count_x, info.root_count_y, info.node_count);
    CHECK(info.level_count == 4);
    CHECK(info.filled_samples > 0);

    // data that has been cut short, or changed where it matters, is refused
    for (const size_t size : {size_t{0}, size_t{63}, size_t{64}, s_data.size() / 2, s_data.size() - 1}) {
        CHECK(!gc::ResourceTerrain::create(std::span<const uint8_t>(s_data.data(), size)).has_value());
    }
    {
        std::vector<uint8_t> wrong_version = s_data;
        wrong_version[0] ^= 0xff;
        CHECK(!gc::ResourceTerrain::create(wrong_version).has_value());
        std::vector<uint8_t> bad_child = s_data;
        const uint32_t huge = 0x7fffffff;
        std::memcpy(bad_child.data() + sizeof(gc::TerrainHeader) + offsetof(gc::TerrainNode, children), &huge, sizeof(huge));
        CHECK(!gc::ResourceTerrain::create(bad_child).has_value());
    }
    const std::optional<gc::ResourceTerrain> created = gc::ResourceTerrain::create(s_data);
    CHECK(created.has_value());
    if (!created) {
        return;
    }
    const gc::ResourceTerrain& terrain = *created;
    CHECK(terrain.header.data_min_x == 0 && terrain.header.data_max_x == WIDTH && terrain.header.data_min_y == 0 && terrain.header.data_max_y == HEIGHT);

    print("Terrains: the tree");
    {
        // Every node's bounds hold its tile and its children. Every tile has a parent, except the top level's
        int wrong = 0;
        std::vector<int> parents(terrain.nodes.size(), 0);
        std::vector<std::pair<gc::TerrainNodeRef, gc::AABB>> stack{};
        for (uint32_t y = 0; y < terrain.header.root_count_y; ++y) {
            for (uint32_t x = 0; x < terrain.header.root_count_x; ++x) {
                const gc::TerrainNodeRef root = terrain.getRoot(x, y);
                stack.emplace_back(root, terrain.getNodeBounds(root));
                parents[root.index] = 1;
            }
        }
        while (!stack.empty()) {
            const auto [node, parent_box] = stack.back();
            stack.pop_back();
            const gc::AABB box = terrain.getNodeBounds(node);
            wrong += (box.min.z >= parent_box.min.z && box.max.z <= parent_box.max.z) ? 0 : 1;
            wrong += (terrain.nodes[node.index].skirt_depth > 0.0f) ? 0 : 1;
            for (const uint32_t sample : terrain.getTile(node.index)) {
                const float h = terrain.decodeHeight(sample);
                wrong += (h >= box.min.z && h <= box.max.z) ? 0 : 1;
            }
            for (uint32_t quarter = 0; quarter < 4 && node.level > 0; ++quarter) {
                const gc::TerrainNodeRef child = terrain.getChild(node, quarter);
                if (child.index != gc::TERRAIN_NO_NODE) {
                    ++parents[child.index];
                    stack.emplace_back(child, box);
                }
            }
        }
        CHECK(wrong == 0);
        CHECK(std::all_of(parents.begin(), parents.end(), [](int count) { return count == 1; }));
        // where there is no data, only the levels from fill_level up are stored
        CHECK(info.nodes_per_level[0] < (WIDTH / 16 + 1) * (HEIGHT / 16 + 1));
    }

    print("Terrains: heights");
    {
        // where there was data, the ground is where the data said, to the precision that heights are stored in
        int wrong = 0;
        for (uint32_t y = 0; y < HEIGHT; y += 3) {
            for (uint32_t x = 0; x < WIDTH; x += 3) {
                const float expected = heights[static_cast<size_t>(y) * WIDTH + x];
                const std::optional<float> h = terrain.getHeight(static_cast<float>(x) * SPACING, static_cast<float>(y) * SPACING);
                if (!h) {
                    ++wrong;
                }
                else if (std::isfinite(expected) && std::abs(*h - expected) > terrain.header.height_scale) {
                    ++wrong;
                }
            }
        }
        CHECK(wrong == 0);
        CHECK(!terrain.getHeight(-1.0f, 5.0f).has_value());
        CHECK(!terrain.getHeight(5.0f, terrain.getSize().y + 1.0f).has_value());
    }

    CHECK(!resource_manager.add<gc::ResourceTerrain>(gc::ResourceTerrain(terrain), "terrain"_name).empty());

    // (components are registered in the same order in every World)
    gc::World world{};
    world.registerComponent<gc::ColliderComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::CollisionBvhComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::TerrainComponent, gc::ComponentArrayType::SPARSE>();
    world.registerSystem<gc::CollisionSystem>(resource_manager);
    gc::CollisionSystem& collision = world.getSystem<gc::CollisionSystem>();
    gc::FrameState frame_state{};

    // moved and turned, as a terrain's entity can be
    const glm::vec3 position{-60.0f, 25.0f, -95.0f};
    const glm::quat rotation = glm::angleAxis(0.4f, glm::vec3{0.0f, 0.0f, 1.0f});
    const gc::Entity entity = world.createEntity("terrain"_name, gc::ENTITY_NONE, position, rotation);
    world.addComponent<gc::TerrainComponent>(entity).setTerrain("terrain"_name);
    const gc::Entity cube = world.createEntity("cube"_name, gc::ENTITY_NONE, glm::vec3{0.0f, 0.0f, 10.0f});
    world.addComponent<gc::ColliderComponent>(cube).setMesh("cube"_name);
    world.update(frame_state);
    CHECK(collision.getTerrainCount() == 1);
    CHECK(collision.getColliderAABB(entity).has_value());

    const glm::mat4 matrix = world.getComponent<gc::TransformComponent>(entity)->getWorldMatrix();
    std::vector<Triangle> triangles = getTerrainTriangles(terrain, matrix, entity);
    const std::vector<Triangle> cube_triangles = getWorldTriangles(world, resource_manager);
    triangles.insert(triangles.end(), cube_triangles.begin(), cube_triangles.end());
    print("    {} triangles", triangles.size());
    const glm::vec2 size = terrain.getSize();
    const auto random_point_over = [&](float min_z, float max_z) {
        const glm::vec3 local{randomFloat(-10.0f, size.x + 10.0f), randomFloat(-10.0f, size.y + 10.0f), randomFloat(min_z, max_z)};
        return glm::vec3(matrix * glm::vec4(local, 1.0f));
    };

    print("Terrains: ray casts");
    {
        constexpr int RAY_COUNT = 400;
        int hits = 0;
        int wrong = 0;
        for (int i = 0; i < RAY_COUNT; ++i) {
            gc::Ray ray{};
            ray.origin = random_point_over(terrain.header.min_height - 20.0f, terrain.header.max_height + 40.0f);
            // straight down, nearly flat (grazing the hills), and anything
            ray.direction = (i % 4 == 0) ? glm::vec3{0.0f, 0.0f, -1.0f}
                                         : ((i % 4 == 1) ? glm::vec3{randomFloat(-1.0f, 1.0f), randomFloat(-1.0f, 1.0f), randomFloat(-0.05f, 0.02f)}
                                                         : randomVec3(-1.0f, 1.0f));
            if (glm::length(ray.direction) < 0.1f) {
                continue;
            }
            const glm::dvec3 direction = glm::normalize(glm::dvec3(ray.direction));
            double expected = std::numeric_limits<double>::max();
            for (const Triangle& triangle : triangles) {
                const double t = slowRayTriangle(glm::dvec3(ray.origin), direction, triangle);
                if (t >= 0.0 && t < expected) {
                    expected = t;
                }
            }
            const bool expected_hit = expected < std::numeric_limits<double>::max();
            const gc::RaycastHit hit = collision.raycast(ray);
            hits += hit.hit ? 1 : 0;
            if (hit.hit != expected_hit) {
                ++wrong;
            }
            else if (hit.hit) {
                const bool distance_ok = std::abs(static_cast<double>(hit.distance) - expected) < 2.0e-2;
                const bool normal_ok = std::abs(glm::length(hit.normal) - 1.0f) < 1.0e-3f && glm::dot(hit.normal, glm::vec3(direction)) <= 1.0e-4f;
                wrong += (distance_ok && normal_ok) ? 0 : 1;
            }
        }
        print("    {} rays, {} hit, {} differ from the slow version", RAY_COUNT, hits, wrong);
        CHECK(wrong <= RAY_COUNT / 200);
        CHECK(hits > RAY_COUNT / 4);
        // a short ray stops short
        const glm::vec3 above = glm::vec3(matrix * glm::vec4(50.0f, 50.0f, terrain.header.max_height + 5.0f, 1.0f));
        CHECK(collision.raycast(gc::Ray{above, glm::vec3{0.0f, 0.0f, -1.0f}}).hit);
        CHECK(!collision.raycast(gc::Ray{above, glm::vec3{0.0f, 0.0f, -1.0f}}, 1.0f).hit);
        CHECK(!collision.raycast(gc::Ray{above, glm::vec3{0.0f, 0.0f, -1.0f}}, 1000.0f, entity).hit);
    }

    print("Terrains: spheres");
    {
        constexpr int SPHERE_COUNT = 200;
        int contact_count = 0;
        int wrong = 0;
        for (int i = 0; i < SPHERE_COUNT; ++i) {
            const glm::vec3 local_xy{randomFloat(0.0f, size.x), randomFloat(0.0f, size.y), 0.0f};
            const float ground = terrain.getHeight(local_xy.x, local_xy.y).value_or(0.0f);
            const float radius = randomFloat(0.2f, 3.0f);
            const glm::vec3 centre = glm::vec3(matrix * glm::vec4(local_xy.x, local_xy.y, ground + randomFloat(-1.0f, 3.0f), 1.0f));
            const float local_z = glm::vec3(glm::inverse(matrix) * glm::vec4(centre, 1.0f)).z;

            std::vector<gc::SphereContact> contacts{};
            collision.overlapSphere(centre, radius, contacts, cube);
            contact_count += static_cast<int>(contacts.size());
            bool ok = contacts.size() <= 1;
            if (local_z < ground) {
                // under the ground: pushed straight up, out of it
                ok = ok && contacts.size() == 1;
                if (!contacts.empty()) {
                    ok = ok && std::abs(contacts[0].depth - (radius + ground - local_z)) < 1.0e-2f;
                    ok = ok && glm::distance(contacts[0].normal, glm::vec3{0.0f, 0.0f, 1.0f}) < 1.0e-3f;
                }
                wrong += ok ? 0 : 1;
                continue;
            }
            double expected = std::numeric_limits<double>::max();
            for (const Triangle& triangle : triangles) {
                if (triangle.entity == entity) {
                    expected = std::min(expected, glm::distance(slowClosestPoint(glm::dvec3(centre), triangle), glm::dvec3(centre)));
                }
            }
            if (expected < static_cast<double>(radius)) {
                ok = ok && contacts.size() == 1;
                if (!contacts.empty()) {
                    ok = ok && std::abs(static_cast<double>(radius - contacts[0].depth) - expected) < 1.0e-2;
                    ok = ok && glm::distance(contacts[0].position + contacts[0].normal * (radius - contacts[0].depth), centre) < 1.0e-2f;
                }
            }
            else {
                ok = ok && (contacts.empty() || contacts[0].depth < 1.0e-3f);
            }
            wrong += ok ? 0 : 1;
        }
        print("    {} spheres, {} contacts, {} differ from the slow version", SPHERE_COUNT, contact_count, wrong);
        CHECK(wrong == 0);
        CHECK(contact_count > SPHERE_COUNT / 4);
    }

    print("Terrains: turning collision off, and removing the terrain");
    world.getComponent<gc::TerrainComponent>(entity)->setCollision(false);
    world.update(frame_state);
    CHECK(collision.getTerrainCount() == 0);
    const glm::vec3 above = glm::vec3(matrix * glm::vec4(50.0f, 50.0f, terrain.header.max_height + 5.0f, 1.0f));
    CHECK(!collision.raycast(gc::Ray{above, glm::vec3{0.0f, 0.0f, -1.0f}}, 500.0f, cube).hit);
    world.getComponent<gc::TerrainComponent>(entity)->setCollision(true);
    world.update(frame_state);
    CHECK(collision.raycast(gc::Ray{above, glm::vec3{0.0f, 0.0f, -1.0f}}, 500.0f, cube).hit);
    world.deleteEntity(entity);
    world.update(frame_state);
    CHECK(collision.getTerrainCount() == 0);
    CHECK(!collision.raycast(gc::Ray{above, glm::vec3{0.0f, 0.0f, -1.0f}}, 500.0f, cube).hit);
}

// --terrain-only skips the tests of meshes
int main(int argc, char* argv[])
{
    const bool terrain_only = (argc > 1 && std::string(argv[1]) == "--terrain-only");

    for (const char* const name :
         {"TransformComponent", "ColliderComponent", "CollisionBvhComponent", "TerrainComponent", "cube", "ball", "plane", "missing", "baked", "baked_plane",
          "bvh", "terrain"}) {
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

    if (terrain_only) {
        testTerrains(resource_manager);
        print("{} ({} of {} checks failed)", (s_failures == 0) ? "PASSED" : "FAILED", s_failures, s_checks);
        return (s_failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    gc::World world{};
    world.registerComponent<gc::ColliderComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::CollisionBvhComponent, gc::ComponentArrayType::SPARSE>();
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

    //
    // Baked trees: the same queries, with the trees of the meshes read from COLLISION_BVH assets instead of built
    //

    const auto removeColliders = [&]() {
        std::vector<gc::Entity> with_colliders{};
        world.forEach<gc::ColliderComponent>([&](gc::Entity entity, const gc::ColliderComponent&) { with_colliders.push_back(entity); });
        for (const gc::Entity entity : with_colliders) {
            world.deleteEntity(entity);
        }
    };
    const auto addColliders = [&]() {
        for (int i = 0; i < 200; ++i) {
            const glm::quat rotation = glm::angleAxis(randomFloat(0.0f, 6.28f), glm::normalize(randomVec3(-1.0f, 1.0f) + glm::vec3{0.0f, 0.0f, 1.5f}));
            const glm::vec3 scale = (i % 3 == 0) ? randomVec3(0.5f, 4.0f) : glm::vec3{randomFloat(0.5f, 3.0f)};
            const gc::Entity entity = world.createEntity("collider"_name, gc::ENTITY_NONE, randomVec3(-18.0f, 18.0f), rotation, scale);
            world.addComponent<gc::ColliderComponent>(entity).setMesh((i % 2 == 0) ? "cube"_name : ((i % 5 == 0) ? "plane"_name : "ball"_name));
        }
    };
    const auto makeBakedData = [&](std::initializer_list<gc::Name> mesh_names) {
        std::vector<std::pair<gc::Name, gc::CollisionBvh>> meshes{};
        for (const gc::Name mesh_name : mesh_names) {
            const gc::ResourceMesh* const mesh = resource_manager.get<gc::ResourceMesh>(mesh_name);
            meshes.emplace_back(mesh_name, gc::buildCollisionBvh(mesh->vertices.get(), mesh->indices.get()));
        }
        return gc::makeCollisionBvhData(meshes);
    };

    print("Baked collision trees: the asset");
    // (the resources are views of this data, so it is kept until the end)
    const std::vector<uint8_t> baked_data = makeBakedData({"cube"_name, "ball"_name});
    const std::vector<uint8_t> baked_plane_data = makeBakedData({"plane"_name, "cube"_name});
    {
        const auto resource = gc::ResourceCollisionBvh::create(baked_data);
        CHECK(resource.has_value());
        CHECK(resource && resource->meshes.size() == 2);
        if (resource && resource->meshes.contains("ball"_name)) {
            // what comes out of the asset is what went in
            const gc::ResourceMesh* const mesh = resource_manager.get<gc::ResourceMesh>("ball"_name);
            const gc::CollisionBvh built = gc::buildCollisionBvh(mesh->vertices.get(), mesh->indices.get());
            const gc::CollisionBvhView& baked = resource->meshes.at("ball"_name);
            CHECK(baked.positions.size() == built.positions.size() && baked.positions.size() == mesh->vertices.get().size());
            CHECK(baked.triangles.size() == built.triangles.size() && baked.triangles.size() == mesh->indices.get().size());
            CHECK(baked.nodes.size() == built.nodes.size() && !baked.nodes.empty());
            CHECK(std::equal(baked.triangles.begin(), baked.triangles.end(), built.triangles.begin(), built.triangles.end()));
            CHECK(std::equal(baked.positions.begin(), baked.positions.end(), built.positions.begin(), built.positions.end()));
            CHECK(std::equal(baked.nodes.begin(), baked.nodes.end(), built.nodes.begin(), built.nodes.end(),
                             [](const gc::CollisionBvhNode& a, const gc::CollisionBvhNode& b) {
                                 return a.box.min == b.box.min && a.box.max == b.box.max && a.first == b.first && a.count == b.count;
                             }));
        }
        // data that has been cut short, or isn't this kind of asset at all, is refused
        for (const size_t size : {size_t{0}, size_t{7}, size_t{24}, baked_data.size() / 2, baked_data.size() - 1}) {
            CHECK(!gc::ResourceCollisionBvh::create(std::span<const uint8_t>(baked_data.data(), size)).has_value());
        }
        std::vector<uint8_t> wrong_version = baked_data;
        wrong_version[0] ^= 0xff;
        CHECK(!gc::ResourceCollisionBvh::create(wrong_version).has_value());
    }
    CHECK(!resource_manager.add<gc::ResourceCollisionBvh>(*gc::ResourceCollisionBvh::create(baked_data), "baked"_name).empty());
    CHECK(!resource_manager.add<gc::ResourceCollisionBvh>(*gc::ResourceCollisionBvh::create(baked_plane_data), "baked_plane"_name).empty());

    print("Baked collision trees: one asset, with two of the three meshes in it");
    const gc::Entity baked_entity = world.createEntity("bvh"_name);
    world.addComponent<gc::CollisionBvhComponent>(baked_entity).setBvh("baked"_name);
    addColliders();
    world.update(frame_state);
    CHECK(collision.getColliderCount() == 200);
    CHECK(collision.getMeshCount() == 3);
    CHECK(collision.getBakedMeshCount() == 2); // the plane's tree is built
    compareRaycasts(world, resource_manager, collision, 600, "ray casts with baked trees");
    compareSpheres(world, resource_manager, collision, 300, "spheres with baked trees");
    compareBoxQueries(world, collision, 300);

    print("Baked collision trees: two assets at once, with a mesh that is in both");
    removeColliders();
    world.update(frame_state);
    CHECK(collision.getMeshCount() == 0);
    const gc::Entity baked_plane_entity = world.createEntity("bvh"_name);
    world.addComponent<gc::CollisionBvhComponent>(baked_plane_entity).setBvh("baked_plane"_name);
    addColliders();
    world.update(frame_state);
    CHECK(collision.getMeshCount() == 3);
    CHECK(collision.getBakedMeshCount() == 3);
    compareRaycasts(world, resource_manager, collision, 600, "ray casts with two baked assets");
    compareSpheres(world, resource_manager, collision, 300, "spheres with two baked assets");

    print("Baked collision trees: an asset is taken away while its trees are in use, and one that doesn't exist (an error is logged)");
    world.deleteEntity(baked_entity);
    world.getComponent<gc::CollisionBvhComponent>(baked_plane_entity)->setBvh("missing"_name);
    world.update(frame_state);
    CHECK(collision.getBakedMeshCount() == 3); // the colliders that were there keep their trees
    compareRaycasts(world, resource_manager, collision, 600, "ray casts after the assets have gone");
    removeColliders();
    world.update(frame_state);
    addColliders();
    world.update(frame_state);
    CHECK(collision.getMeshCount() == 3);
    CHECK(collision.getBakedMeshCount() == 0); // new colliders have theirs built
    compareRaycasts(world, resource_manager, collision, 600, "ray casts with built trees again");
    compareSpheres(world, resource_manager, collision, 300, "spheres with built trees again");

    testTerrains(resource_manager);

    if (s_failures == 0) {
        print("PASSED ({} checks)", s_checks);
        return EXIT_SUCCESS;
    }
    print("FAILED ({} of {} checks)", s_failures, s_checks);
    return EXIT_FAILURE;
}
