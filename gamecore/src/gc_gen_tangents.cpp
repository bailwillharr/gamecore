#include <gamecore/gc_gen_tangents.h>

#include <variant>

#include <mikktspace.h>
#include <weldmesh.h>

#include <gamecore/gc_render_mesh.h>

namespace gc {

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

std::vector<int> genTangents(std::vector<gc::MeshVertex>& vertices)
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

    const int new_vertex_count = WeldMesh(remap_table.data(), reinterpret_cast<float*>(vertex_data_out.data()), reinterpret_cast<float*>(vertices.data()),
                                              static_cast<int>(vertices.size()), MeshVertex::floatsPerVertex());
    GC_ASSERT(new_vertex_count >= 0);

    // get new vertices into the vector.
    // This potentially modifies the size of the input 'vector' argument
    vertices.resize(static_cast<size_t>(new_vertex_count));
    for (size_t i = 0; i < static_cast<size_t>(new_vertex_count); ++i) {
        vertices[i] = vertex_data_out[i];
    }

    return remap_table;
}

}