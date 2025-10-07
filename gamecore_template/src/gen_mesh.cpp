#include "gen_mesh.h"

#include <gtc/constants.hpp>
#include <geometric.hpp>

#include <gamecore/gc_render_backend.h>
#include <gamecore/gc_gen_tangents.h>

gc::RenderMesh genCuboidMesh(gc::RenderBackend& render_backend, float x, float y, float z, float t, bool wind_inside)
{
    std::vector<gc::MeshVertex> vertices{};
    vertices.reserve(36);

    // XY plane (+Z normal) (TOP)
    vertices.push_back({{0, 0, z}, {0, 0, 1}, {}, {0, 0}}); // bottom left
    vertices.push_back({{x, 0, z}, {0, 0, 1}, {}, {t, 0}}); // bottom right
    vertices.push_back({{0, y, z}, {0, 0, 1}, {}, {0, t}}); // top left
    vertices.push_back({{0, y, z}, {0, 0, 1}, {}, {0, t}}); // top left
    vertices.push_back({{x, 0, z}, {0, 0, 1}, {}, {t, 0}}); // bottom right
    vertices.push_back({{x, y, z}, {0, 0, 1}, {}, {t, t}}); // top right
    // XY plane (-Z normal) (BOTTOM)
    vertices.push_back({{x, 0, 0}, {0, 0, -1}, {}, {t, t}}); // bottom right
    vertices.push_back({{0, 0, 0}, {0, 0, -1}, {}, {0, t}}); // bottom left
    vertices.push_back({{0, y, 0}, {0, 0, -1}, {}, {0, 0}}); // top left
    vertices.push_back({{x, 0, 0}, {0, 0, -1}, {}, {t, t}}); // bottom right
    vertices.push_back({{0, y, 0}, {0, 0, -1}, {}, {0, 0}}); // top left
    vertices.push_back({{x, y, 0}, {0, 0, -1}, {}, {t, 0}}); // top right
    // XZ plane (+Y normal) (BACK)
    vertices.push_back({{x, y, 0}, {0, 1, 0}, {}, {0, 0}}); // bottom right
    vertices.push_back({{0, y, 0}, {0, 1, 0}, {}, {t, 0}}); // bottom left
    vertices.push_back({{0, y, z}, {0, 1, 0}, {}, {t, t}}); // top left
    vertices.push_back({{x, y, 0}, {0, 1, 0}, {}, {0, 0}}); // bottom right
    vertices.push_back({{0, y, z}, {0, 1, 0}, {}, {t, t}}); // top left
    vertices.push_back({{x, y, z}, {0, 1, 0}, {}, {0, t}}); // top right
    // XZ plane (-Y normal) (FRONT)
    vertices.push_back({{0, 0, 0}, {0, -1, 0}, {}, {0, 0}}); // bottom left
    vertices.push_back({{x, 0, 0}, {0, -1, 0}, {}, {t, 0}}); // bottom right
    vertices.push_back({{0, 0, z}, {0, -1, 0}, {}, {0, t}}); // top left
    vertices.push_back({{0, 0, z}, {0, -1, 0}, {}, {0, t}}); // top left
    vertices.push_back({{x, 0, 0}, {0, -1, 0}, {}, {t, 0}}); // bottom right
    vertices.push_back({{x, 0, z}, {0, -1, 0}, {}, {t, t}}); // top right
    // YZ plane (+X normal) (RIGHT)
    vertices.push_back({{x, 0, 0}, {1, 0, 0}, {}, {0, 0}}); // bottom left
    vertices.push_back({{x, y, 0}, {1, 0, 0}, {}, {t, 0}}); // bottom right
    vertices.push_back({{x, 0, z}, {1, 0, 0}, {}, {0, t}}); // top left
    vertices.push_back({{x, 0, z}, {1, 0, 0}, {}, {0, t}}); // top left
    vertices.push_back({{x, y, 0}, {1, 0, 0}, {}, {t, 0}}); // bottom right
    vertices.push_back({{x, y, z}, {1, 0, 0}, {}, {t, t}}); // top right
    // YZ plane (-X normal) (LEFT)
    vertices.push_back({{0, y, 0}, {-1, 0, 0}, {}, {0, 0}}); // bottom right
    vertices.push_back({{0, 0, 0}, {-1, 0, 0}, {}, {t, 0}}); // bottom left
    vertices.push_back({{0, 0, z}, {-1, 0, 0}, {}, {t, t}}); // top left
    vertices.push_back({{0, y, 0}, {-1, 0, 0}, {}, {0, 0}}); // bottom right
    vertices.push_back({{0, 0, z}, {-1, 0, 0}, {}, {t, t}}); // top left
    vertices.push_back({{0, y, z}, {-1, 0, 0}, {}, {0, t}}); // top right

#if 0
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
#endif

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

    auto indices_int32 = gc::genTangents(vertices);
    std::vector<uint16_t> indices{};
    indices.reserve(indices_int32.size());
    for (uint32_t index : indices_int32) {
        GC_ASSERT(index <= UINT16_MAX);
        indices.push_back(static_cast<uint16_t>(index));
    }

    return render_backend.createMesh(vertices, indices);
}

gc::RenderMesh genSphereMesh(gc::RenderBackend& render_backend, float r, int detail, bool flip_normals)
{
    using namespace glm;

    std::vector<gc::MeshVertex> vertices{};

    const float angle_step = two_pi<float>() / (float)detail;

    for (int i = 0; i < detail; i++) {
        // theta goes north-to-south
        float theta = i * angle_step;
        float theta2 = theta + angle_step;
        const float v2 = theta / glm::two_pi<float>();
        const float v1 = theta2 / glm::two_pi<float>();
        for (int j = 0; j < detail / 2; j++) {
            // phi goes west-to-east
            float phi = j * angle_step;
            float phi2 = phi + angle_step;

            vec3 top_left{r * sin(phi) * cos(theta), r * cos(phi), r * sin(phi) * sin(theta)};
            vec3 bottom_left{r * sin(phi) * cos(theta2), r * cos(phi), r * sin(phi) * sin(theta2)};
            vec3 top_right{r * sin(phi2) * cos(theta), r * cos(phi2), r * sin(phi2) * sin(theta)};
            vec3 bottom_right{r * sin(phi2) * cos(theta2), r * cos(phi2), r * sin(phi2) * sin(theta2)};

            const float u1 = phi / glm::pi<float>();
            const float u2 = phi2 / glm::pi<float>();

            // triangle 1
            vertices.push_back({top_left, {}, {}, {u1, v2}});
            vertices.push_back({bottom_left, {}, {}, {u1, v1}});
            vertices.push_back({bottom_right, {}, {}, {u2, v1}});
            // triangle 2
            vertices.push_back({top_left, {}, {}, {u1, v2}});
            vertices.push_back({bottom_right, {}, {}, {u2, v1}});
            vertices.push_back({top_right, {}, {}, {u2, v2}});

            for (auto it = vertices.end() - 6; it != vertices.end(); ++it) {
                it->normal = normalize(it->position);
                if (flip_normals) {
                    it->normal = -it->normal;
                }
            }
        }
    }

    auto indices_int32 = gc::genTangents(vertices);
    std::vector<uint16_t> indices{};
    indices.reserve(indices_int32.size());
    for (uint32_t index : indices_int32) {
        GC_ASSERT(index <= UINT16_MAX);
        indices.push_back(static_cast<uint16_t>(index));
    }

    return render_backend.createMesh(vertices, indices);
}
