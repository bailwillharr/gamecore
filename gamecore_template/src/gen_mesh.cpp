#include "gen_mesh.h"

#include <gtc/constants.hpp>
#include <geometric.hpp>

#include <gamecore/gc_render_backend.h>

gc::RenderMesh genCuboidMesh(gc::RenderBackend& render_backend, float x, float y, float z, float tiling, bool wind_inside)
{
    std::vector<gc::MeshVertex> vertices{};
    vertices.reserve(36);

    // front
    vertices.push_back({{0.0f, 0.0f, z}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}});
    vertices.push_back({{0.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, tiling}});
    vertices.push_back({{x, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {tiling, tiling}});
    vertices.push_back({{x, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {tiling, tiling}});
    vertices.push_back({{x, 0.0f, z}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {tiling, 0.0f}});
    vertices.push_back({{0.0f, 0.0f, z}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}});
    // back
    vertices.push_back({{x, y, z}, {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}});
    vertices.push_back({{x, y, 0.0f}, {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, tiling}});
    vertices.push_back({{0.0f, y, 0.0f}, {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}, {tiling, tiling}});
    vertices.push_back({{0.0f, y, 0.0f}, {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}, {tiling, tiling}});
    vertices.push_back({{0.0f, y, z}, {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}, {tiling, 0.0f}});
    vertices.push_back({{x, y, z}, {0.0f, 1.0f, 0.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}});
    // left
    vertices.push_back({{0.0f, y, z}, {-1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f, 1.0f}, {0.0f, 0.0f}});
    vertices.push_back({{0.0f, y, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f, 1.0f}, {0.0f, tiling}});
    vertices.push_back({{0.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f, 1.0f}, {tiling, tiling}});
    vertices.push_back({{0.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f, 1.0f}, {tiling, tiling}});
    vertices.push_back({{0.0f, 0.0f, z}, {-1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f, 1.0f}, {tiling, 0.0f}});
    vertices.push_back({{0.0f, y, z}, {-1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f, 1.0f}, {0.0f, 0.0f}});
    // right
    vertices.push_back({{x, 0.0f, z}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 0.0f}});
    vertices.push_back({{x, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, {0.0f, tiling}});
    vertices.push_back({{x, y, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, {tiling, tiling}});
    vertices.push_back({{x, y, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, {tiling, tiling}});
    vertices.push_back({{x, y, z}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, {tiling, 0.0f}});
    vertices.push_back({{x, 0.0f, z}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 0.0f}});
    // top
    vertices.push_back({{0.0f, y, z}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}});
    vertices.push_back({{0.0f, 0.0f, z}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, tiling}});
    vertices.push_back({{x, 0.0f, z}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {tiling, tiling}});
    vertices.push_back({{x, 0.0f, z}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {tiling, tiling}});
    vertices.push_back({{x, y, z}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {tiling, 0.0f}});
    vertices.push_back({{0.0f, y, z}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}});
    // bottom
    vertices.push_back({{x, y, 0.0f}, {0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}});
    vertices.push_back({{x, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, tiling}});
    vertices.push_back({{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}, {tiling, tiling}});
    vertices.push_back({{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}, {tiling, tiling}});
    vertices.push_back({{0.0f, y, 0.0f}, {0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}, {tiling, 0.0f}});
    vertices.push_back({{x, y, 0.0f}, {0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}});

    // centre the positions
    for (auto& v : vertices) {
        v.position.x -= x * 0.5f;
        v.position.y -= y * 0.5f;
        v.position.z -= z * 0.5f;
    }

    if (wind_inside) {
        for (size_t i = 0; i < vertices.size(); i += 3) {
            std::swap(vertices[i], vertices[i + 2]);
        }
    }

    std::vector<uint16_t> indices{};
    indices.reserve(vertices.size());
    for (uint16_t i = 0; i < static_cast<uint16_t>(vertices.size()); ++i) {
        indices.push_back(i);
    }

    return render_backend.createMesh(vertices, indices);
}

gc::RenderMesh genSphereMesh(gc::RenderBackend& render_backend, float r, int detail, bool wind_inside, bool flip_normals)
{
    using namespace glm;

    std::vector<gc::MeshVertex> vertices{};

    const float angle_step = two_pi<float>() / (float)detail;

    for (int i = 0; i < detail; i++) {
        // theta goes north-to-south
        float theta = i * angle_step;
        float theta2 = theta + angle_step;
        for (int j = 0; j < detail / 2; j++) {
            // phi goes west-to-east
            float phi = j * angle_step;
            float phi2 = phi + angle_step;

            vec3 top_left{r * sin(phi) * cos(theta), r * cos(phi), r * sin(phi) * sin(theta)};
            vec3 bottom_left{r * sin(phi) * cos(theta2), r * cos(phi), r * sin(phi) * sin(theta2)};
            vec3 top_right{r * sin(phi2) * cos(theta), r * cos(phi2), r * sin(phi2) * sin(theta)};
            vec3 bottom_right{r * sin(phi2) * cos(theta2), r * cos(phi2), r * sin(phi2) * sin(theta2)};

            if (wind_inside == false) {
                // tris are visible from outside the sphere

                // triangle 1
                vertices.push_back({top_left, {}, {}, {0.0f, 0.0f}});
                vertices.push_back({bottom_left, {}, {}, {0.0f, 1.0f}});
                vertices.push_back({bottom_right, {}, {}, {1.0f, 1.0f}});
                // triangle 2
                vertices.push_back({top_right, {}, {}, {1.0f, 0.0f}});
                vertices.push_back({top_left, {}, {}, {0.0f, 0.0f}});
                vertices.push_back({bottom_right, {}, {}, {1.0f, 1.0f}});
            }
            else {
                // tris are visible from inside the sphere

                // triangle 1
                vertices.push_back({bottom_right, {}, {}, {1.0f, 1.0f}});
                vertices.push_back({bottom_left, {}, {}, {0.0f, 1.0f}});
                vertices.push_back({top_left, {}, {}, {0.0f, 0.0f}});

                // triangle 2
                vertices.push_back({bottom_right, {}, {}, {1.0f, 1.0f}});
                vertices.push_back({top_left, {}, {}, {0.0f, 0.0f}});
                vertices.push_back({top_right, {}, {}, {1.0f, 0.0f}});
            }

            vec3 vector1 = (vertices.end() - 1)->position - (vertices.end() - 2)->position;
            vec3 vector2 = (vertices.end() - 2)->position - (vertices.end() - 3)->position;
            vec3 norm = normalize(cross(vector2, vector1));

            // NORMALS HAVE BEEN FIXED

            if (flip_normals) norm = -norm;

            if (j == (detail / 2) - 1) norm = -norm;

            for (auto it = vertices.end() - 6; it != vertices.end(); ++it) {
                it->normal = norm;
            }
        }
    }

    std::vector<uint16_t> indices{};
    indices.reserve(vertices.size());
    for (uint16_t i = 0; i < static_cast<uint16_t>(vertices.size()); ++i) {
        indices.push_back(i);
    }

    return render_backend.createMesh(vertices, indices);
}