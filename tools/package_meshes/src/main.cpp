//
// package_meshes.exe
//

#define _CRT_SECURE_NO_WARNINGS

#include "package_meshes.h"

#include <cstring>

#include <iostream>
#include <vector>
#include <filesystem>
#include <memory>
#include <algorithm>
#include <string_view>
#include <span>

#include <glm.hpp>

#include <mikktspace.h>
#include <weldmesh.h>

#include <gcpak/gcpak.h>

#include <gctemplates/gct_sv_stream.h>

#include <mio/mmap.hpp>

struct MeshVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec4 tangent;
    glm::vec2 uv;

    static consteval int floatsPerVertex() { return static_cast<int>(sizeof(position) + sizeof(normal) + sizeof(tangent) + sizeof(uv)) / 4; }
};

[[noreturn]] static void abortProgram(std::string_view sv)
{
    std::cerr << sv << "\n";
    abort();
}

static int getNumFaces(const SMikkTSpaceContext* pContext)
{
    auto vertices = static_cast<const std::vector<MeshVertex>*>(pContext->m_pUserData);
    return static_cast<int>(vertices->size() / 3);
}

static int getNumVerticesOfFace(const SMikkTSpaceContext*, int) { return 3; }

static void getPosition(const SMikkTSpaceContext* pContext, float fvPosOut[], const int iFace, const int iVert)
{
    auto vertices = static_cast<const std::vector<MeshVertex>*>(pContext->m_pUserData);
    const size_t i = static_cast<size_t>(iFace) * 3 + static_cast<size_t>(iVert);
    const glm::vec3 pos = (*vertices)[i].position;
    fvPosOut[0] = pos.x;
    fvPosOut[1] = pos.y;
    fvPosOut[2] = pos.z;
}

static void getNormal(const SMikkTSpaceContext* pContext, float fvNormOut[], const int iFace, const int iVert)
{
    auto vertices = static_cast<const std::vector<MeshVertex>*>(pContext->m_pUserData);
    const size_t i = static_cast<size_t>(iFace) * 3 + static_cast<size_t>(iVert);
    const glm::vec3 norm = (*vertices)[i].normal;
    fvNormOut[0] = norm.x;
    fvNormOut[1] = norm.y;
    fvNormOut[2] = norm.z;
}

static void getTexCoord(const SMikkTSpaceContext* pContext, float fvTexcOut[], const int iFace, const int iVert)
{
    auto vertices = static_cast<const std::vector<MeshVertex>*>(pContext->m_pUserData);
    const size_t i = static_cast<size_t>(iFace) * 3 + static_cast<size_t>(iVert);
    const glm::vec2 uv = (*vertices)[i].uv;
    fvTexcOut[0] = uv.x;
    fvTexcOut[1] = uv.y;
}

static void setTSpaceBasic(const SMikkTSpaceContext* pContext, const float fvTangent[], const float fSign, const int iFace, const int iVert)
{
    auto vertices = static_cast<std::vector<MeshVertex>*>(pContext->m_pUserData);
    const size_t i = static_cast<size_t>(iFace) * 3 + static_cast<size_t>(iVert);
    (*vertices)[i].tangent.x = fvTangent[0];
    (*vertices)[i].tangent.y = fvTangent[1];
    (*vertices)[i].tangent.z = fvTangent[2];
    (*vertices)[i].tangent.w = fSign;
}

static std::vector<int> genTangents(std::vector<MeshVertex>& vertices)
{
    assert(vertices.size() % 3 == 0);
    static_assert(sizeof(MeshVertex) == 12 * sizeof(float));

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
        abortProgram("Failed to generate tangents");
    }

    // generate new vertex and index list without duplicates:

    std::vector<int> remap_table(vertices.size());            // initialised to zeros
    std::vector<MeshVertex> vertex_data_out(vertices.size()); // initialised to zeros

    const int new_vertex_count = WeldMesh(remap_table.data(), reinterpret_cast<float*>(vertex_data_out.data()), reinterpret_cast<float*>(vertices.data()),
                                          static_cast<int>(vertices.size()), MeshVertex::floatsPerVertex());
    assert(new_vertex_count >= 0);

    // get new vertices into the vector.
    // This potentially modifies the size of the input 'vector' argument
    vertices.resize(static_cast<size_t>(new_vertex_count));
    for (size_t i = 0; i < static_cast<size_t>(new_vertex_count); ++i) {
        vertices[i] = vertex_data_out[i];
    }

    return remap_table;
}

static void parseV(const std::string& line, std::vector<glm::vec3>& positions)
{
    float x{}, y{}, z{};
    if (sscanf(line.c_str(), "v %f %f %f", &x, &y, &z) != 3) {
        abortProgram("scanf error");
    }
    // use Z-up instead
    glm::vec3 pos{x, -z, y};
    positions.push_back(pos);
}

static void parseT(const std::string& line, std::vector<glm::vec2>& uvs)
{
    float u{}, v{};
    if (sscanf(line.c_str(), "vt %f %f", &u, &v) != 2) {
        abortProgram("scanf error");
    }
    glm::vec2 uv{u, v};
    uvs.push_back(uv);
}

static void parseN(const std::string& line, std::vector<glm::vec3>& normals)
{
    float x{}, y{}, z{};
    if (sscanf(line.c_str(), "vn %f %f %f", &x, &y, &z) != 3) {
        abortProgram("scanf error");
    }
    // use Z-up instead
    glm::vec3 normal{x, -z, y};
    normal = glm::normalize(normal);
    normals.push_back(normal);
}

static void parseF(const std::string& line, std::span<const glm::vec3> positions, std::span<const glm::vec2> uvs, std::span<const glm::vec3> normals,
                   std::vector<MeshVertex>& vertices)
{
    int indices[3][3];
    if (sscanf(line.c_str(), "f %d/%d/%d %d/%d/%d %d/%d/%d", &indices[0][0], &indices[0][1], &indices[0][2], &indices[1][0], &indices[1][1], &indices[1][2],
               &indices[2][0], &indices[2][1], &indices[2][2]) != 9) {
        abortProgram("scanf error");
    }

    // per face
    for (int i = 0; i < 3; ++i) {
        for (int comp = 0; comp < 3; ++comp) {
            if (indices[i][comp] < 0) {
                abortProgram("Don't support negative indices");
            }
        }
        const int pos_index = indices[i][0] - 1;
        if (pos_index >= positions.size()) {
            abortProgram("Invalid pos index");
        }
        const int uv_index = indices[i][1] - 1;
        if (uv_index >= uvs.size()) {
            abortProgram("Invalid uv index");
        }
        const int norm_index = indices[i][2] - 1;
        if (norm_index >= normals.size()) {
            abortProgram("Invalid normal index");
        }

        MeshVertex v{};
        v.position = positions[pos_index];
        v.uv = uvs[uv_index];
        v.normal = normals[norm_index];
        vertices.push_back(v);
    }
}

static std::vector<uint8_t> loadOBJMesh(std::span<const uint8_t> file_data)
{
    gct::sv_istream stream(std::string_view(reinterpret_cast<const char*>(file_data.data()), file_data.size()));

    std::string line{};

    std::vector<glm::vec3> positions{};
    std::vector<glm::vec2> uvs{};
    std::vector<glm::vec3> normals{};
    std::vector<MeshVertex> vertices{};

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

    const auto indices_int32 = genTangents(vertices);
    std::vector<uint16_t> indices{};
    indices.reserve(indices_int32.size());
    for (uint32_t index : indices_int32) {
        assert(index <= UINT16_MAX);
        indices.push_back(static_cast<uint16_t>(index));
    }

    const size_t output_size = sizeof(uint16_t) + vertices.size() * sizeof(MeshVertex) + indices.size() * sizeof(uint16_t);
    std::vector<uint8_t> output(output_size);

    const uint16_t num_vertices = static_cast<uint16_t>(vertices.size());
    std::memcpy(output.data(), &num_vertices, sizeof(uint16_t));
    std::memcpy(output.data() + sizeof(uint16_t), vertices.data(), num_vertices * sizeof(MeshVertex));
    std::memcpy(output.data() + sizeof(uint16_t) + num_vertices * sizeof(MeshVertex), indices.data(), indices.size() * sizeof(uint16_t));

    return output;
}

static bool isMesh(const std::filesystem::path& path)
{
    auto ext = path.extension().string();
    std::transform(ext.cbegin(), ext.cend(), ext.begin(), [](char c) -> char { return static_cast<char>(tolower(c)); });
    return (ext == ".obj");
}

// empty on failure
static std::vector<uint8_t> readMesh(const std::filesystem::path& path)
{
    std::error_code err;
    mio::ummap_source file{};
    file.map(path.string(), err);
    if (err) {
        abortProgram("Failed to map file");
    }
    return loadOBJMesh(file);
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    std::error_code ec{};

    const auto mesh_dir = std::filesystem::path(PACKAGE_MESHES_SOURCE_DIRECTORY).parent_path().parent_path() / "content" / "meshes";
    if (!std::filesystem::exists(mesh_dir, ec) || !std::filesystem::is_directory(mesh_dir, ec)) {
        std::cerr << "Failed to find meshes directory! error: " << ec.message() << "\n";
        return EXIT_FAILURE;
    }

    const auto gcpak_path = mesh_dir.parent_path() / "meshes.gcpak";

    // find all mesh files and add them
    gcpak::GcpakCreator gcpak_creator{};
    for (const auto& dir_entry : std::filesystem::directory_iterator(mesh_dir)) {

        if (!dir_entry.is_regular_file()) {
            continue;
        }

        if (!isMesh(dir_entry.path())) {
            continue;
        }

        auto data = readMesh(dir_entry.path());
        if (data.empty()) {
            std::cerr << "Failed to read mesh: " << dir_entry.path().filename() << "\n";
            continue;
        }

        std::cout << "Adding mesh: " << dir_entry.path().filename() << "\n";
        gcpak_creator.addAsset(
            gcpak::GcpakCreator::Asset{dir_entry.path().filename().string(), 0, data, gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16});
    }

    if (!gcpak_creator.saveFile(gcpak_path)) {
        std::cerr << "Failed to save gcpak file " << gcpak_path.filename() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "Saved meshes to " << gcpak_path << "\n";

    { // wait for enter before exit
        std::cout << "Press enter to exit\n";
        int c{};
        do {
            c = getchar();
        } while (c != '\n' && c != EOF);
    }

    return EXIT_SUCCESS;
}
