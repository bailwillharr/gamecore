#include "gltf_compiler.h"

#include <cctype>
#include <cmath>
#include <cstring>

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>

#include <stb_image.h>
#include <tiny_gltf.h>

#include <gclog/gclog.h>

#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_gen_tangents.h>
#include <gamecore/gc_light_component.h>
#include <gamecore/gc_prefab.h>
#include <gamecore/gc_renderable_component.h>

namespace tg = tinygltf;

namespace {

constexpr size_t MAX_MESH_VERTICES = UINT16_MAX;
constexpr int MAX_NODE_DEPTH = 256;

struct Pixels {
    uint32_t width{};
    uint32_t height{};
    std::vector<uint8_t> rgba{};
};

// One draw: a mesh asset and the material to draw it with
struct Draw {
    gc::Name mesh{};
    gc::Name material{};
};

// Where the elements of an accessor are in a buffer. Everything about it has been checked to be in bounds.
struct AccessorView {
    const uint8_t* data{};
    size_t stride{};
    size_t count{};
    int component_type{};
    int num_components{};
    bool normalized{};
};

class GltfCompiler {
    const tg::Model& m_model;
    const std::filesystem::path m_directory; // that the file is in. External images are relative to it
    const std::string m_name;                // the file's name. Every asset's name starts with it

    std::vector<Asset> m_assets{};

    std::vector<std::unique_ptr<Pixels>> m_images{}; // decoded when first needed. Null if not decoded yet
    std::vector<bool> m_images_failed{};
    std::unordered_map<std::string, gc::Name> m_textures{};             // by a description of what is in the texture
    std::unordered_map<int, gc::Name> m_materials{};                    // by glTF material index. -1 is the default material
    std::unordered_map<int, std::vector<Draw>> m_meshes{};              // by glTF mesh index
    std::vector<bool> m_nodes_visited{};

    gc::PrefabWriter m_prefab{};

public:
    GltfCompiler(const tg::Model& model, const std::filesystem::path& path)
        : m_model(model),
          m_directory(path.parent_path()),
          m_name(path.filename().string()),
          m_images(model.images.size()),
          m_images_failed(model.images.size(), false),
          m_nodes_visited(model.nodes.size(), false)
    {
    }

    bool compile(const std::filesystem::path& path, std::vector<Asset>& assets_out)
    {
        std::vector<int> root_nodes{};
        if (!m_model.scenes.empty()) {
            const int scene_index = (m_model.defaultScene >= 0 && m_model.defaultScene < static_cast<int>(m_model.scenes.size())) ? m_model.defaultScene : 0;
            root_nodes = m_model.scenes[scene_index].nodes;
            if (m_model.scenes.size() > 1) {
                GC_WARN("{}: has {} scenes. Only scene {} is compiled", m_name, m_model.scenes.size(), scene_index);
            }
        }
        else {
            // no scenes. Use every node that isn't a child of another one
            std::vector<bool> is_child(m_model.nodes.size(), false);
            for (const tg::Node& node : m_model.nodes) {
                for (const int child : node.children) {
                    if (child >= 0 && child < static_cast<int>(is_child.size())) {
                        is_child[child] = true;
                    }
                }
            }
            for (int i = 0; i < static_cast<int>(m_model.nodes.size()); ++i) {
                if (!is_child[i]) {
                    root_nodes.push_back(i);
                }
            }
        }
        if (root_nodes.empty()) {
            GC_ERROR("{}: has no nodes", m_name);
            return false;
        }

        if (!m_model.animations.empty() || !m_model.skins.empty()) {
            GC_WARN("{}: animations and skins aren't supported and are left out", m_name);
        }

        // glTF uses the Y-up convention, so everything goes under an entity that rotates it to Z-up
        const uint32_t root =
            m_prefab.beginEntity(gc::Name(path.stem().string()), gcpak::PREFAB_NO_PARENT, glm::vec3{0.0f, 0.0f, 0.0f},
                                 glm::quat{glm::one_over_root_two<float>(), glm::one_over_root_two<float>(), 0.0f, 0.0f});
        for (const int node_index : root_nodes) {
            addNode(node_index, root, 0);
        }

        const auto prefab_data = m_prefab.getData();
        m_assets.push_back(makeAsset(m_name, std::vector<uint8_t>(prefab_data.begin(), prefab_data.end()), gcpak::GcpakAssetType::PREFAB));

        GC_DEBUG("{}: {} entities", m_name, m_prefab.getEntityCount());
        for (Asset& asset : m_assets) {
            assets_out.push_back(std::move(asset));
        }
        return true;
    }

private:
    //
    // Textures
    //

    // null if the image can't be used
    const Pixels* getImage(int image_index)
    {
        if (image_index < 0 || image_index >= static_cast<int>(m_model.images.size()) || m_images_failed[image_index]) {
            return nullptr;
        }
        if (m_images[image_index]) {
            return m_images[image_index].get();
        }

        const tg::Image& image = m_model.images[image_index];
        auto pixels = std::make_unique<Pixels>();

        if (!image.image.empty()) {
            // embedded in the file, and already decoded by tinygltf
            if (image.as_is || image.bits != 8 || image.component != 4 || image.pixel_type != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE || image.width <= 0 ||
                image.height <= 0 || image.image.size() != static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4) {
                GC_ERROR("{}: image {} is in an unsupported format. Images must have 8 bits per channel", m_name, image_index);
                m_images_failed[image_index] = true;
                return nullptr;
            }
            pixels->width = static_cast<uint32_t>(image.width);
            pixels->height = static_cast<uint32_t>(image.height);
            pixels->rgba = image.image;
        }
        else if (!image.uri.empty()) {
            // a separate file
            const std::filesystem::path image_path = m_directory / decodeUri(image.uri);
            int width{}, height{}, channels_in_file{};
            const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> data(stbi_load(image_path.string().c_str(), &width, &height, &channels_in_file, 4),
                                                                             stbi_image_free);
            if (!data || width <= 0 || height <= 0) {
                GC_ERROR("{}: failed to read image {}: {}", m_name, image_path.string(), stbi_failure_reason());
                m_images_failed[image_index] = true;
                return nullptr;
            }
            pixels->width = static_cast<uint32_t>(width);
            pixels->height = static_cast<uint32_t>(height);
            pixels->rgba.assign(data.get(), data.get() + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        }
        else {
            GC_ERROR("{}: image {} has no data", m_name, image_index);
            m_images_failed[image_index] = true;
            return nullptr;
        }

        m_images[image_index] = std::move(pixels);
        return m_images[image_index].get();
    }

    // -1 if the texture has no usable image
    int getTextureImageIndex(const std::string& material_name, const char* what, int texture_index, int tex_coord)
    {
        if (texture_index < 0 || texture_index >= static_cast<int>(m_model.textures.size())) {
            return -1;
        }
        if (tex_coord != 0) {
            GC_WARN("{}: the {} texture of material '{}' uses UV channel {}. Only channel 0 is supported, so the texture is left out", m_name, what,
                    material_name, tex_coord);
            return -1;
        }
        const int image_index = m_model.textures[texture_index].source;
        return getImage(image_index) ? image_index : -1;
    }

    static std::string decodeUri(const std::string& uri)
    {
        std::string decoded{};
        for (size_t i = 0; i < uri.size(); ++i) {
            if (uri[i] == '%' && i + 2 < uri.size() && std::isxdigit(static_cast<unsigned char>(uri[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(uri[i + 2]))) {
                decoded.push_back(static_cast<char>(std::stoi(uri.substr(i + 1, 2), nullptr, 16)));
                i += 2;
            }
            else {
                decoded.push_back(uri[i]);
            }
        }
        return decoded;
    }

    static float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
    static float linearToSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }
    static uint8_t toByte(float c) { return static_cast<uint8_t>(std::lround(glm::clamp(c, 0.0f, 1.0f) * 255.0f)); }

    static float getFactor(const std::vector<double>& factors, size_t index)
    {
        return index < factors.size() ? glm::clamp(static_cast<float>(factors[index]), 0.0f, 1.0f) : 1.0f;
    }

    // Adds a texture asset, unless one with the same description was already made.
    template <typename MakePixels>
    gc::Name getTexture(const std::string& description, bool srgb, MakePixels&& make_pixels)
    {
        if (const auto it = m_textures.find(description); it != m_textures.end()) {
            return it->second;
        }
        const Pixels pixels = make_pixels();
        const std::string asset_name = std::format("{}/texture{}", m_name, m_textures.size());
        m_assets.push_back(makeAsset(asset_name, makeTextureData(pixels.width, pixels.height, pixels.rgba),
                                     srgb ? gcpak::GcpakAssetType::TEXTURE_R8G8B8A8_SRGB : gcpak::GcpakAssetType::TEXTURE_R8G8B8A8));
        const gc::Name name(asset_name);
        m_textures.emplace(description, name);
        return name;
    }

    // The engine's materials have no constant factors, so they are baked into the textures. Without a texture, the factor becomes
    // a single pixel.
    gc::Name getBaseColorTexture(const tg::Material& material)
    {
        const auto& pbr = material.pbrMetallicRoughness;
        const std::array<float, 4> factor{getFactor(pbr.baseColorFactor, 0), getFactor(pbr.baseColorFactor, 1), getFactor(pbr.baseColorFactor, 2),
                                          getFactor(pbr.baseColorFactor, 3)};
        const bool has_factor = factor != std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f};
        const int image_index = getTextureImageIndex(material.name, "base color", pbr.baseColorTexture.index, pbr.baseColorTexture.texCoord);

        const std::string description = std::format("base {} {} {} {} {}", image_index, factor[0], factor[1], factor[2], factor[3]);
        return getTexture(description, true, [&]() {
            Pixels pixels{};
            if (image_index < 0) {
                // the factor is a linear color, textures hold sRGB colors
                pixels.width = 1;
                pixels.height = 1;
                pixels.rgba = {toByte(linearToSrgb(factor[0])), toByte(linearToSrgb(factor[1])), toByte(linearToSrgb(factor[2])), toByte(factor[3])};
                return pixels;
            }
            pixels = *getImage(image_index);
            if (has_factor) {
                std::array<std::array<uint8_t, 256>, 4> tables{};
                for (int value = 0; value < 256; ++value) {
                    const float c = static_cast<float>(value) / 255.0f;
                    for (int channel = 0; channel < 3; ++channel) {
                        tables[channel][value] = toByte(linearToSrgb(srgbToLinear(c) * factor[channel]));
                    }
                    tables[3][value] = toByte(c * factor[3]);
                }
                for (size_t i = 0; i < pixels.rgba.size(); ++i) {
                    pixels.rgba[i] = tables[i % 4][pixels.rgba[i]];
                }
            }
            return pixels;
        });
    }

    // Occlusion in red, roughness in green, metallic in blue. glTF uses the same channels, but allows occlusion to be a separate image.
    gc::Name getOrmTexture(const tg::Material& material)
    {
        const auto& pbr = material.pbrMetallicRoughness;
        const float roughness = glm::clamp(static_cast<float>(pbr.roughnessFactor), 0.0f, 1.0f);
        const float metallic = glm::clamp(static_cast<float>(pbr.metallicFactor), 0.0f, 1.0f);
        const int mr_image_index =
            getTextureImageIndex(material.name, "metallic-roughness", pbr.metallicRoughnessTexture.index, pbr.metallicRoughnessTexture.texCoord);
        int occlusion_image_index = getTextureImageIndex(material.name, "occlusion", material.occlusionTexture.index, material.occlusionTexture.texCoord);

        if (mr_image_index >= 0 && occlusion_image_index >= 0 && occlusion_image_index != mr_image_index) {
            const Pixels* const mr = getImage(mr_image_index);
            const Pixels* const occlusion = getImage(occlusion_image_index);
            if (mr->width != occlusion->width || mr->height != occlusion->height) {
                GC_WARN("{}: the occlusion texture of material '{}' is a different size to its metallic-roughness texture, so it is left out", m_name,
                        material.name);
                occlusion_image_index = -1;
            }
        }

        const std::string description = std::format("orm {} {} {} {}", mr_image_index, occlusion_image_index, roughness, metallic);
        return getTexture(description, false, [&]() {
            Pixels pixels{};
            const Pixels* const mr = getImage(mr_image_index);
            const Pixels* const occlusion = getImage(occlusion_image_index);
            if (!mr && !occlusion) {
                pixels.width = 1;
                pixels.height = 1;
                pixels.rgba = {255, toByte(roughness), toByte(metallic), 255};
                return pixels;
            }
            const Pixels& size_source = mr ? *mr : *occlusion;
            pixels.width = size_source.width;
            pixels.height = size_source.height;
            pixels.rgba.resize(size_source.rgba.size());
            for (size_t i = 0; i < pixels.rgba.size(); i += 4) {
                pixels.rgba[i + 0] = occlusion ? occlusion->rgba[i + 0] : uint8_t{255};
                pixels.rgba[i + 1] = toByte((mr ? static_cast<float>(mr->rgba[i + 1]) / 255.0f : 1.0f) * roughness);
                pixels.rgba[i + 2] = toByte((mr ? static_cast<float>(mr->rgba[i + 2]) / 255.0f : 1.0f) * metallic);
                pixels.rgba[i + 3] = 255;
            }
            return pixels;
        });
    }

    // empty if the material has no normal map, which makes the engine use a flat one
    gc::Name getNormalTexture(const tg::Material& material)
    {
        const int image_index = getTextureImageIndex(material.name, "normal", material.normalTexture.index, material.normalTexture.texCoord);
        if (image_index < 0) {
            return {};
        }
        if (material.normalTexture.scale != 1.0) {
            GC_WARN("{}: the normal texture scale of material '{}' is ignored", m_name, material.name);
        }
        return getTexture(std::format("normal {}", image_index), false, [&]() { return *getImage(image_index); });
    }

    //
    // Materials
    //

    gc::Name getMaterial(int material_index)
    {
        if (material_index < 0 || material_index >= static_cast<int>(m_model.materials.size())) {
            material_index = -1;
        }
        if (const auto it = m_materials.find(material_index); it != m_materials.end()) {
            return it->second;
        }

        // glTF's default material is what an empty material is
        static const tg::Material s_default_material{};
        const tg::Material& material = (material_index >= 0) ? m_model.materials[material_index] : s_default_material;

        if (material.alphaMode != "OPAQUE") {
            GC_WARN("{}: material '{}' uses alpha mode {}. Transparency isn't supported, so it will be opaque", m_name, material.name, material.alphaMode);
        }
        if (material.emissiveTexture.index >= 0 ||
            std::any_of(material.emissiveFactor.begin(), material.emissiveFactor.end(), [](double factor) { return factor != 0.0; })) {
            GC_WARN("{}: material '{}' is emissive, which isn't supported", m_name, material.name);
        }

        const gc::Name base_color = getBaseColorTexture(material);
        const gc::Name orm = getOrmTexture(material);
        const gc::Name normal = getNormalTexture(material);

        const std::string asset_name = (material_index >= 0) ? std::format("{}/material{}", m_name, material_index) : std::format("{}/material_default", m_name);
        m_assets.push_back(makeAsset(asset_name, makeMaterialData(base_color, orm, normal), gcpak::GcpakAssetType::MATERIAL));
        const gc::Name name(asset_name);
        m_materials.emplace(material_index, name);
        return name;
    }

    //
    // Meshes
    //

    std::optional<AccessorView> getAccessorView(int accessor_index, int expected_type) const
    {
        if (accessor_index < 0 || accessor_index >= static_cast<int>(m_model.accessors.size())) {
            return {};
        }
        const tg::Accessor& accessor = m_model.accessors[accessor_index];
        if (accessor.type != expected_type || accessor.count == 0 || accessor.sparse.isSparse) {
            return {};
        }
        if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(m_model.bufferViews.size())) {
            return {};
        }
        const tg::BufferView& buffer_view = m_model.bufferViews[accessor.bufferView];
        if (buffer_view.buffer < 0 || buffer_view.buffer >= static_cast<int>(m_model.buffers.size())) {
            return {};
        }
        const tg::Buffer& buffer = m_model.buffers[buffer_view.buffer];

        const int component_size = tg::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType));
        const int num_components = tg::GetNumComponentsInType(static_cast<uint32_t>(accessor.type));
        const int stride = accessor.ByteStride(buffer_view);
        if (component_size <= 0 || num_components <= 0 || stride <= 0) {
            return {};
        }
        const size_t offset = buffer_view.byteOffset + accessor.byteOffset;
        const size_t element_size = static_cast<size_t>(component_size) * static_cast<size_t>(num_components);
        const size_t end = offset + static_cast<size_t>(stride) * (accessor.count - 1) + element_size;
        if (end > buffer.data.size()) {
            return {};
        }

        AccessorView view{};
        view.data = buffer.data.data() + offset;
        view.stride = static_cast<size_t>(stride);
        view.count = accessor.count;
        view.component_type = accessor.componentType;
        view.num_components = num_components;
        view.normalized = accessor.normalized;
        return view;
    }

    static float readFloat(const AccessorView& view, size_t element, int component)
    {
        const uint8_t* const location = view.data + view.stride * element;
        switch (view.component_type) {
        case TINYGLTF_COMPONENT_TYPE_FLOAT: {
            float value{};
            std::memcpy(&value, location + sizeof(float) * component, sizeof(float));
            return value;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: {
            const uint8_t value = location[component];
            return view.normalized ? static_cast<float>(value) / 255.0f : static_cast<float>(value);
        }
        case TINYGLTF_COMPONENT_TYPE_BYTE: {
            int8_t value{};
            std::memcpy(&value, location + component, sizeof(int8_t));
            return view.normalized ? glm::max(static_cast<float>(value) / 127.0f, -1.0f) : static_cast<float>(value);
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
            uint16_t value{};
            std::memcpy(&value, location + sizeof(uint16_t) * component, sizeof(uint16_t));
            return view.normalized ? static_cast<float>(value) / 65535.0f : static_cast<float>(value);
        }
        case TINYGLTF_COMPONENT_TYPE_SHORT: {
            int16_t value{};
            std::memcpy(&value, location + sizeof(int16_t) * component, sizeof(int16_t));
            return view.normalized ? glm::max(static_cast<float>(value) / 32767.0f, -1.0f) : static_cast<float>(value);
        }
        default:
            return 0.0f;
        }
    }

    static uint32_t readIndex(const AccessorView& view, size_t element)
    {
        const uint8_t* const location = view.data + view.stride * element;
        switch (view.component_type) {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            return *location;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: {
            uint16_t value{};
            std::memcpy(&value, location, sizeof(uint16_t));
            return value;
        }
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: {
            uint32_t value{};
            std::memcpy(&value, location, sizeof(uint32_t));
            return value;
        }
        default:
            return UINT32_MAX;
        }
    }

    static glm::vec3 anyPerpendicular(const glm::vec3& normal)
    {
        const glm::vec3 other = (std::abs(normal.z) < 0.9f) ? glm::vec3{0.0f, 0.0f, 1.0f} : glm::vec3{1.0f, 0.0f, 0.0f};
        return glm::normalize(glm::cross(other, normal));
    }

    // Returns false if the primitive can't be used. Otherwise vertices and indices are a triangle list.
    bool readPrimitive(const tg::Primitive& primitive, const std::string& where, std::vector<gc::MeshVertex>& vertices, std::vector<uint32_t>& indices) const
    {
        if (primitive.mode != TINYGLTF_MODE_TRIANGLES && primitive.mode != -1) {
            GC_WARN("{}: {} isn't a triangle list (mode {}), so it is left out", m_name, where, primitive.mode);
            return false;
        }

        const auto getAttribute = [&](const char* attribute_name, int type) -> std::optional<AccessorView> {
            const auto it = primitive.attributes.find(attribute_name);
            if (it == primitive.attributes.end()) {
                return {};
            }
            auto view = getAccessorView(it->second, type);
            if (!view) {
                GC_WARN("{}: the {} attribute of {} is invalid or of an unsupported kind, so it is ignored", m_name, attribute_name, where);
            }
            return view;
        };

        const auto positions = getAttribute("POSITION", TINYGLTF_TYPE_VEC3);
        if (!positions || positions->component_type != TINYGLTF_COMPONENT_TYPE_FLOAT) {
            GC_WARN("{}: {} has no usable positions, so it is left out", m_name, where);
            return false;
        }
        const size_t vertex_count = positions->count;

        // other attributes are only used if there is one for every vertex
        const auto getVertexAttribute = [&](const char* attribute_name, int type) {
            auto view = getAttribute(attribute_name, type);
            if (view && view->count < vertex_count) {
                view.reset();
            }
            return view;
        };
        const auto normals = getVertexAttribute("NORMAL", TINYGLTF_TYPE_VEC3);
        const auto uvs = getVertexAttribute("TEXCOORD_0", TINYGLTF_TYPE_VEC2);
        const auto tangents = getVertexAttribute("TANGENT", TINYGLTF_TYPE_VEC4);

        indices.clear();
        if (primitive.indices >= 0) {
            const auto index_view = getAccessorView(primitive.indices, TINYGLTF_TYPE_SCALAR);
            if (!index_view) {
                GC_WARN("{}: {} has invalid indices, so it is left out", m_name, where);
                return false;
            }
            indices.reserve(index_view->count);
            for (size_t i = 0; i < index_view->count; ++i) {
                const uint32_t index = readIndex(*index_view, i);
                if (index >= vertex_count) {
                    GC_WARN("{}: {} has an index that is out of range, so it is left out", m_name, where);
                    return false;
                }
                indices.push_back(index);
            }
        }
        else {
            indices.resize(vertex_count);
            std::iota(indices.begin(), indices.end(), 0u);
        }
        indices.resize(indices.size() - indices.size() % 3);
        if (indices.empty()) {
            return false;
        }

        const auto readVertex = [&](size_t i) {
            gc::MeshVertex vertex{};
            vertex.position = {readFloat(*positions, i, 0), readFloat(*positions, i, 1), readFloat(*positions, i, 2)};
            if (normals) {
                vertex.normal = {readFloat(*normals, i, 0), readFloat(*normals, i, 1), readFloat(*normals, i, 2)};
            }
            if (uvs) {
                // Not flipped. glTF's UV origin is the top-left of the image, and the images are stored the same way round.
                vertex.uv = {readFloat(*uvs, i, 0), readFloat(*uvs, i, 1)};
            }
            if (tangents) {
                vertex.tangent = {readFloat(*tangents, i, 0), readFloat(*tangents, i, 1), readFloat(*tangents, i, 2), readFloat(*tangents, i, 3)};
            }
            return vertex;
        };

        vertices.clear();
        if (normals && tangents) {
            // everything is there, use the vertices as they are
            vertices.reserve(vertex_count);
            for (size_t i = 0; i < vertex_count; ++i) {
                vertices.push_back(readVertex(i));
            }
            return true;
        }

        // Tangents have to be generated, which is done on separate triangles
        vertices.reserve(indices.size());
        for (const uint32_t index : indices) {
            vertices.push_back(readVertex(index));
        }
        if (!normals) {
            // flat shading
            for (size_t i = 0; i < vertices.size(); i += 3) {
                glm::vec3 normal = glm::cross(vertices[i + 1].position - vertices[i].position, vertices[i + 2].position - vertices[i].position);
                const float length = glm::length(normal);
                normal = (length > 0.0f) ? normal / length : glm::vec3{0.0f, 0.0f, 1.0f};
                vertices[i].normal = vertices[i + 1].normal = vertices[i + 2].normal = normal;
            }
        }
        const std::vector<int> remap = gc::genTangents(vertices); // also merges identical vertices
        for (size_t i = 0; i < indices.size(); ++i) {
            indices[i] = static_cast<uint32_t>(remap[i]);
        }
        // without UVs there is nothing to base the tangents on, and any will do
        for (gc::MeshVertex& vertex : vertices) {
            const glm::vec3 tangent{vertex.tangent};
            if (!(glm::dot(tangent, tangent) > 1.0e-12f)) {
                vertex.tangent = glm::vec4(anyPerpendicular(vertex.normal), 1.0f);
            }
        }
        return true;
    }

    // Mesh assets have 16 bit indices, so a big mesh is split into several.
    void addMeshAssets(const std::vector<gc::MeshVertex>& vertices, const std::vector<uint32_t>& indices, const std::string& asset_name, gc::Name material,
                       std::vector<Draw>& draws)
    {
        const auto addPart = [&](std::span<const gc::MeshVertex> part_vertices, std::span<const uint16_t> part_indices, const std::string& part_name) {
            m_assets.push_back(makeAsset(part_name, makeMeshData(part_vertices, part_indices), gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16));
            draws.push_back(Draw{gc::Name(part_name), material});
        };

        if (vertices.size() <= MAX_MESH_VERTICES) {
            const std::vector<uint16_t> small_indices(indices.begin(), indices.end());
            addPart(vertices, small_indices, asset_name);
            return;
        }

        std::vector<uint32_t> remap(vertices.size(), UINT32_MAX); // index in the current part
        std::vector<gc::MeshVertex> part_vertices{};
        std::vector<uint32_t> part_sources{}; // which vertex each of the part's vertices came from
        std::vector<uint16_t> part_indices{};
        int part_count{};
        const auto finishPart = [&]() {
            addPart(part_vertices, part_indices, std::format("{}_part{}", asset_name, part_count++));
            for (const uint32_t source : part_sources) {
                remap[source] = UINT32_MAX;
            }
            part_vertices.clear();
            part_sources.clear();
            part_indices.clear();
        };
        for (size_t i = 0; i < indices.size(); i += 3) {
            size_t new_vertices{};
            for (size_t corner = 0; corner < 3; ++corner) {
                new_vertices += (remap[indices[i + corner]] == UINT32_MAX) ? 1 : 0;
            }
            if (part_vertices.size() + new_vertices > MAX_MESH_VERTICES) {
                finishPart();
            }
            for (size_t corner = 0; corner < 3; ++corner) {
                const uint32_t source = indices[i + corner];
                if (remap[source] == UINT32_MAX) {
                    remap[source] = static_cast<uint32_t>(part_vertices.size());
                    part_vertices.push_back(vertices[source]);
                    part_sources.push_back(source);
                }
                part_indices.push_back(static_cast<uint16_t>(remap[source]));
            }
        }
        if (!part_indices.empty()) {
            finishPart();
        }
        GC_DEBUG("{}: split {} ({} vertices) into {} meshes", m_name, asset_name, vertices.size(), part_count);
    }

    const std::vector<Draw>& getMesh(int mesh_index)
    {
        if (const auto it = m_meshes.find(mesh_index); it != m_meshes.end()) {
            return it->second;
        }
        std::vector<Draw> draws{};
        const tg::Mesh& mesh = m_model.meshes[mesh_index];
        std::vector<gc::MeshVertex> vertices{};
        std::vector<uint32_t> indices{};
        for (size_t primitive_index = 0; primitive_index < mesh.primitives.size(); ++primitive_index) {
            const tg::Primitive& primitive = mesh.primitives[primitive_index];
            const std::string where = std::format("primitive {} of mesh {} '{}'", primitive_index, mesh_index, mesh.name);
            if (!readPrimitive(primitive, where, vertices, indices)) {
                continue;
            }
            if (!primitive.targets.empty()) {
                GC_WARN("{}: the morph targets of {} are ignored", m_name, where);
            }
            addMeshAssets(vertices, indices, std::format("{}/mesh{}_{}", m_name, mesh_index, primitive_index), getMaterial(primitive.material), draws);
        }
        return m_meshes.emplace(mesh_index, std::move(draws)).first->second;
    }

    //
    // Nodes
    //

    static void decomposeTransform(glm::mat4 transform, glm::vec3& position, glm::quat& rotation, glm::vec3& scale)
    {
        position = glm::vec3(transform[3]);
        transform[3] = glm::vec4{0.0f, 0.0f, 0.0f, 1.0f};

        scale.x = glm::length(glm::vec3(transform[0]));
        scale.y = glm::length(glm::vec3(transform[1]));
        scale.z = glm::length(glm::vec3(transform[2]));
        if (glm::determinant(glm::mat3(transform)) < 0.0f) {
            scale.x = -scale.x; // a mirrored transform. A rotation can't represent that, so one of the scales has to be negative
        }

        for (int axis = 0; axis < 3; ++axis) {
            if (scale[axis] != 0.0f) {
                transform[axis] /= scale[axis];
            }
        }
        rotation = glm::normalize(glm::quat_cast(glm::mat3(transform)));
    }

    void addNode(int node_index, uint32_t parent, int depth)
    {
        if (node_index < 0 || node_index >= static_cast<int>(m_model.nodes.size())) {
            GC_WARN("{}: reference to node {}, which doesn't exist", m_name, node_index);
            return;
        }
        if (m_nodes_visited[node_index] || depth > MAX_NODE_DEPTH) {
            GC_WARN("{}: node {} is used more than once or the hierarchy is too deep, so it is left out", m_name, node_index);
            return;
        }
        m_nodes_visited[node_index] = true;

        const tg::Node& node = m_model.nodes[node_index];
        const std::string node_name = node.name.empty() ? std::format("node{}", node_index) : node.name;

        glm::vec3 position{0.0f, 0.0f, 0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 scale{1.0f, 1.0f, 1.0f};
        if (node.matrix.size() == 16) {
            glm::mat4 matrix{};
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    matrix[column][row] = static_cast<float>(node.matrix[static_cast<size_t>(column) * 4 + row]);
                }
            }
            decomposeTransform(matrix, position, rotation, scale);
        }
        else {
            if (node.translation.size() == 3) {
                position = {static_cast<float>(node.translation[0]), static_cast<float>(node.translation[1]), static_cast<float>(node.translation[2])};
            }
            if (node.rotation.size() == 4) {
                // glTF stores x, y, z, w
                rotation = glm::quat{static_cast<float>(node.rotation[3]), static_cast<float>(node.rotation[0]), static_cast<float>(node.rotation[1]),
                                     static_cast<float>(node.rotation[2])};
                rotation = (glm::dot(rotation, rotation) > 0.0f) ? glm::normalize(rotation) : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
            }
            if (node.scale.size() == 3) {
                scale = {static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]), static_cast<float>(node.scale[2])};
            }
        }

        // The meshes are compiled first, as the prefab's components must directly follow their entity.
        static const std::vector<Draw> s_no_draws{};
        const std::vector<Draw>& draws = (node.mesh >= 0 && node.mesh < static_cast<int>(m_model.meshes.size())) ? getMesh(node.mesh) : s_no_draws;

        const uint32_t entity = m_prefab.beginEntity(gc::Name(node_name), parent, position, rotation, scale);

        if (draws.size() == 1) {
            m_prefab.addComponent(gc::RenderableComponent{}.setMesh(draws[0].mesh).setMaterial(draws[0].material));
        }

        if (node.camera >= 0 && node.camera < static_cast<int>(m_model.cameras.size())) {
            const tg::Camera& camera = m_model.cameras[node.camera];
            if (camera.type == "perspective" && camera.perspective.yfov > 0.0 && camera.perspective.znear > 0.0) {
                // glTF cameras look down their -Z axis with +Y up, as the engine's do.
                // Not active, as instantiating a prefab shouldn't take the view away from the game's camera.
                m_prefab.addComponent(gc::CameraComponent{}
                                          .setFOV(static_cast<float>(camera.perspective.yfov))
                                          .setNearPlane(static_cast<float>(camera.perspective.znear))
                                          .setActive(false));
            }
            else {
                GC_WARN("{}: the camera of node '{}' isn't a perspective camera, so it is left out", m_name, node_name);
            }
        }

        if (node.light >= 0) {
            m_prefab.addComponent(gc::LightComponent{});
        }

        if (draws.size() > 1) {
            // an entity can only have one renderable
            for (size_t i = 0; i < draws.size(); ++i) {
                m_prefab.beginEntity(gc::Name(std::format("{}_mesh{}", node_name, i)), entity);
                m_prefab.addComponent(gc::RenderableComponent{}.setMesh(draws[i].mesh).setMaterial(draws[i].material));
            }
        }

        for (const int child : node.children) {
            addNode(child, entity, depth + 1);
        }
    }
};

} // namespace

bool compileGltf(const std::filesystem::path& path, std::vector<Asset>& assets_out)
{
    const std::string filename = path.filename().string();

    tg::TinyGLTF loader{};
    tg::Model model{};
    std::string error{};
    std::string warning{};

    std::string extension = path.extension().string();
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const bool success = (extension == ".glb") ? loader.LoadBinaryFromFile(&model, &error, &warning, path.string())
                                               : loader.LoadASCIIFromFile(&model, &error, &warning, path.string());
    if (!warning.empty()) {
        GC_WARN("{}: {}", filename, warning);
    }
    if (!success) {
        GC_ERROR("Failed to load {}: {}", filename, error.empty() ? "unknown error" : error);
        return false;
    }

    for (const std::string& required_extension : model.extensionsRequired) {
        if (required_extension == "KHR_draco_mesh_compression" || required_extension == "EXT_meshopt_compression" ||
            required_extension == "KHR_mesh_quantization" || required_extension == "KHR_texture_basisu") {
            GC_ERROR("{}: requires the extension {}, which isn't supported", filename, required_extension);
            return false;
        }
        if (required_extension != "KHR_lights_punctual") {
            GC_WARN("{}: requires the extension {}, which isn't supported. The result may look wrong", filename, required_extension);
        }
    }

    GltfCompiler compiler(model, path);
    return compiler.compile(path, assets_out);
}
