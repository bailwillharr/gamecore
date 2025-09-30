#include "gen_mesh.h"

#include <gtc/constants.hpp>
#include <geometric.hpp>

#include <mikktspace.h>
#include <weldmesh.h>

#include <gamecore/gc_render_backend.h>

static int getNumFaces(const SMikkTSpaceContext* pContext)
{
    auto vertices = static_cast<const std::vector<gc::MeshVertex>*>(pContext->m_pUserData);
    return static_cast<int>(vertices->size() / 3);
}

static int getNumVerticesOfFace(const SMikkTSpaceContext*, int) { return 3; }

static void getPosition(const SMikkTSpaceContext* pContext, float fvPosOut[], const int iFace, const int iVert)
{
    auto vertices = static_cast<const std::vector<gc::MeshVertex>*>(pContext->m_pUserData);
    const size_t i = static_cast<size_t>(iFace) * 3 + static_cast<size_t>(iVert);
    const glm::vec3 pos = (*vertices)[i].position;
    fvPosOut[0] = pos.x;
    fvPosOut[1] = pos.y;
    fvPosOut[2] = pos.z;
}

static void getNormal(const SMikkTSpaceContext* pContext, float fvNormOut[], const int iFace, const int iVert)
{
    auto vertices = static_cast<const std::vector<gc::MeshVertex>*>(pContext->m_pUserData);
    const size_t i = static_cast<size_t>(iFace) * 3 + static_cast<size_t>(iVert);
    const glm::vec3 norm = (*vertices)[i].normal;
    fvNormOut[0] = norm.x;
    fvNormOut[1] = norm.y;
    fvNormOut[2] = norm.z;
}

static void getTexCoord(const SMikkTSpaceContext* pContext, float fvTexcOut[], const int iFace, const int iVert)
{
    auto vertices = static_cast<const std::vector<gc::MeshVertex>*>(pContext->m_pUserData);
    const size_t i = static_cast<size_t>(iFace) * 3 + static_cast<size_t>(iVert);
    const glm::vec2 uv = (*vertices)[i].uv;
    fvTexcOut[0] = uv.x;
    fvTexcOut[1] = uv.y;
}

static void setTSpaceBasic(const SMikkTSpaceContext* pContext, const float fvTangent[], const float fSign, const int iFace, const int iVert)
{
    auto vertices = static_cast<std::vector<gc::MeshVertex>*>(pContext->m_pUserData);
    const size_t i = static_cast<size_t>(iFace) * 3 + static_cast<size_t>(iVert);
    (*vertices)[i].tangent.x = fvTangent[0];
    (*vertices)[i].tangent.y = fvTangent[1];
    (*vertices)[i].tangent.z = fvTangent[2];
    (*vertices)[i].tangent.w = fSign;
}

static std::vector<int> genTangents(std::vector<gc::MeshVertex>& vertices)
{
    GC_ASSERT(vertices.size() % 3 == 0);
    static_assert(sizeof(gc::MeshVertex) == 12 * sizeof(float));

    SMikkTSpaceInterface mts_interface{};
    mts_interface.m_getNumFaces = getNumFaces;
    mts_interface.m_getNumVerticesOfFace = getNumVerticesOfFace;
    mts_interface.m_getPosition = getPosition;
    mts_interface.m_getNormal = getNormal;
    mts_interface.m_getTexCoord = getTexCoord;
    mts_interface.m_setTSpaceBasic = setTSpaceBasic;
    SMikkTSpaceContext mts_context{};
    mts_context.m_pInterface = &mts_interface;
    mts_context.m_pUserData = &vertices;

    if (!genTangSpaceDefault(&mts_context)) {
        gc::abortGame("Failed to generate tangents");
    }

    // generate new vertex and index list without duplicates:

    std::vector<int> remap_table(vertices.size());                // initialised to zeros
    std::vector<gc::MeshVertex> vertex_data_out(vertices.size()); // initialised to zeros

    const int new_vertex_count = WeldMesh(reinterpret_cast<int*>(remap_table.data()), reinterpret_cast<float*>(vertex_data_out.data()),
                                          reinterpret_cast<float*>(vertices.data()), static_cast<int>(vertices.size()), 12);
    GC_ASSERT(new_vertex_count >= 0);

    // get new vertices into the vector.
    // This potentially modifies the size of the input 'vector' argument
    vertices.resize(static_cast<size_t>(new_vertex_count));
    for (size_t i = 0; i < static_cast<size_t>(new_vertex_count); ++i) {
        vertices[i] = vertex_data_out[i];
    }

    return remap_table;
}

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

    auto indices_int32 = genTangents(vertices);
    std::vector<uint16_t> indices{};
    indices.reserve(indices_int32.size());
    for (uint32_t index : indices_int32) {
        GC_ASSERT(index <= UINT16_MAX);
        indices.push_back(static_cast<uint16_t>(index));
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

    auto indices_int32 = genTangents(vertices);
    std::vector<uint16_t> indices{};
    indices.reserve(indices_int32.size());
    for (uint32_t index : indices_int32) {
        GC_ASSERT(index <= UINT16_MAX);
        indices.push_back(static_cast<uint16_t>(index));
    }

    return render_backend.createMesh(vertices, indices);
}
