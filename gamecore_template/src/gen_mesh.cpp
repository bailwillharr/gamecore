#define _CRT_SECURE_NO_WARNINGS

#include "gen_mesh.h"

#include <string_view>
#include <ranges>

#include <gtc/constants.hpp>
#include <geometric.hpp>

#include <gctemplates/gct_sv_stream.h>

#include <gamecore/gc_abort.h>
#include <gamecore/gc_render_backend.h>
#include <gamecore/gc_gen_tangents.h>

static void parseV(const std::string& line, std::vector<glm::vec3>& positions)
{
    float x{}, y{}, z{};
    if (sscanf(line.c_str(), "v %f %f %f", &x, &y, &z) != 3) {
        gc::abortGame("scanf error");
    }
    // use Z-up instead
    glm::vec3 pos{x, -z, y};
    positions.push_back(pos);
}

static void parseT(const std::string& line, std::vector<glm::vec2>& uvs)
{
    float u{}, v{};
    if (sscanf(line.c_str(), "vt %f %f", &u, &v) != 2) {
        gc::abortGame("scanf error");
    }
    glm::vec2 uv{u, v};
    uvs.push_back(uv);
}

static void parseN(const std::string& line, std::vector<glm::vec3>& normals)
{
    float x{}, y{}, z{};
    if (sscanf(line.c_str(), "vn %f %f %f", &x, &y, &z) != 3) {
        gc::abortGame("scanf error");
    }
    // use Z-up instead
    glm::vec3 normal{x, -z, y};
    normal = glm::normalize(normal);
    normals.push_back(normal);
}

static void parseF(const std::string& line, std::span<const glm::vec3> positions, std::span<const glm::vec2> uvs, std::span<const glm::vec3> normals,
                   std::vector<gc::MeshVertex>& vertices)
{
    int indices[3][3];
    if (sscanf(line.c_str(), "f %d/%d/%d %d/%d/%d %d/%d/%d", &indices[0][0], &indices[0][1], &indices[0][2], &indices[1][0], &indices[1][1], &indices[1][2],
               &indices[2][0], &indices[2][1], &indices[2][2]) != 9) {
        gc::abortGame("scanf error");
    }

    // per face
    for (int i = 0; i < 3; ++i) {
        for (int comp = 0; comp < 3; ++comp) {
            if (indices[i][comp] < 0) {
                gc::abortGame("Don't support negative indices");
            }
        }
        const int pos_index = indices[i][0] - 1;
        if (pos_index >= positions.size()) {
            gc::abortGame("Invalid pos index");
        }
        const int uv_index = indices[i][1] - 1;
        if (uv_index >= uvs.size()) {
            gc::abortGame("Invalid uv index");
        }
        const int norm_index = indices[i][2] - 1;
        if (norm_index >= normals.size()) {
            gc::abortGame("Invalid normal index");
        }

        gc::MeshVertex v{};
        v.position = positions[pos_index];
        v.uv = uvs[uv_index];
        v.normal = normals[norm_index];
        vertices.push_back(v);
    }
}

gc::RenderMesh genOBJMesh(gc::RenderBackend& render_backend, std::span<const uint8_t> file_data)
{
    gct::sv_istream stream(std::string_view(reinterpret_cast<const char*>(file_data.data()), file_data.size()));

    std::string line{};

    std::vector<glm::vec3> positions{};
    std::vector<glm::vec2> uvs{};
    std::vector<glm::vec3> normals{};
    std::vector<gc::MeshVertex> vertices{};

    while (std::getline(stream, line)) {
        switch (line[0]) {
            case 'v':
                switch (line[1]) {
                    case ' ':
                        // position
                        parseV(line, positions);
                        break;
                    case 't':
                        // UV
                        parseT(line, uvs);
                        break;
                    case 'n':
                        // normal
                        parseN(line, normals);
                        break;
                }
                break;
            case 'f':
                parseF(line, positions, uvs, normals, vertices);
                break;
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

gc::RenderMesh genPlaneMesh(gc::RenderBackend& render_backend, float t)
{
    using namespace glm;

    std::vector<gc::MeshVertex> vertices{};
    vertices.reserve(6);

    // XY plane (+Z normal) (TOP)
    vertices.emplace_back(vec3{-0.5, -0.5, 0.5}, vec3{0, 0, 1}, vec4{}, vec2{0, 0}); // bottom left
    vertices.emplace_back(vec3{0.5, -0.5, 0.5}, vec3{0, 0, 1}, vec4{}, vec2{t, 0});  // bottom right
    vertices.emplace_back(vec3{-0.5, 0.5, 0.5}, vec3{0, 0, 1}, vec4{}, vec2{0, t});  // top left
    vertices.emplace_back(vec3{-0.5, 0.5, 0.5}, vec3{0, 0, 1}, vec4{}, vec2{0, t});  // top left
    vertices.emplace_back(vec3{0.5, -0.5, 0.5}, vec3{0, 0, 1}, vec4{}, vec2{t, 0});  // bottom right
    vertices.emplace_back(vec3{0.5, 0.5, 0.5}, vec3{0, 0, 1}, vec4{}, vec2{t, t});   // top right

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
        // theta goes west to east
        float theta = i * angle_step;
        float theta2 = theta + angle_step;
        const float u_west = theta / glm::two_pi<float>();
        const float u_east = theta2 / glm::two_pi<float>();
        for (int j = 0; j < detail / 2; j++) {
            // phi goes north to south
            float phi = j * angle_step;
            float phi2 = phi + angle_step;

            vec3 north_west{r * sin(phi) * cos(theta), r * sin(phi) * sin(theta), r * cos(phi)};
            vec3 north_east{r * sin(phi) * cos(theta2), r * sin(phi) * sin(theta2), r * cos(phi)};
            vec3 south_west{r * sin(phi2) * cos(theta), r * sin(phi2) * sin(theta), r * cos(phi2)};
            vec3 south_east{r * sin(phi2) * cos(theta2), r * sin(phi2) * sin(theta2), r * cos(phi2)};

            const float v_north = 1.0f - (phi / glm::pi<float>());
            const float v_south = 1.0f - (phi2 / glm::pi<float>());

            // triangle 1
            vertices.push_back({north_west, {}, {}, {u_west, v_north}});
            vertices.push_back({south_west, {}, {}, {u_west, v_south}});
            vertices.push_back({south_east, {}, {}, {u_east, v_south}});
            // triangle 2
            vertices.push_back({south_east, {}, {}, {u_east, v_south}});
            vertices.push_back({north_east, {}, {}, {u_east, v_north}});
            vertices.push_back({north_west, {}, {}, {u_west, v_north}});

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
