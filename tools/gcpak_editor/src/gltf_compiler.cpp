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
#include <gamecore/gc_collider_component.h>
#include <gamecore/gc_gen_tangents.h>
#include <gamecore/gc_light_component.h>
#include <gamecore/gc_prefab.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_shadow_map_component.h>
#include <gamecore/gc_stopwatch.h>

#include "shadow_baker.h"

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

    // For baking the directional light's shadow map, see bakeShadows()
    std::vector<glm::mat4> m_entity_matrices{};                       // of every entity in the prefab, in prefab space
    std::unordered_map<gc::Name, size_t> m_mesh_assets{};             // where each mesh asset is in m_assets
    std::unordered_map<gc::Name, size_t> m_texture_assets{};          // where each texture asset is in m_assets
    std::unordered_map<gc::Name, gc::ResourceMaterial> m_material_resources{}; // what each material asset was made from
    std::vector<std::pair<Draw, glm::mat4>> m_shadow_casters{};       // everything that is drawn, and where
    std::optional<glm::vec3> m_direction_to_light{};                  // of the directional light, in prefab space

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
        const glm::quat root_rotation{glm::one_over_root_two<float>(), glm::one_over_root_two<float>(), 0.0f, 0.0f};
        const uint32_t root = m_prefab.beginEntity(gc::Name(path.stem().string()), gcpak::PREFAB_NO_PARENT, glm::vec3{0.0f, 0.0f, 0.0f}, root_rotation);
        m_entity_matrices.push_back(glm::mat4_cast(root_rotation));
        for (const int node_index : root_nodes) {
            addNode(node_index, root, 0);
        }

        bakeShadows();

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
        m_texture_assets.emplace(gc::Name(asset_name), m_assets.size());
        m_assets.push_back(makeAsset(asset_name, makeTextureData(pixels.width, pixels.height, pixels.rgba),
                                     srgb ? gcpak::GcpakAssetType::TEXTURE_R8G8B8A8_SRGB : gcpak::GcpakAssetType::TEXTURE_R8G8B8A8));
        const gc::Name name(asset_name);
        m_textures.emplace(description, name);
        return name;
    }

    // The engine uses a material's constants in place of a texture, not together with one, so a texture has the factors baked
    // into it. Without a texture there is nothing to bake: these return an empty name and the factors become the material's
    // constants (see getMaterial()), which lets the engine draw it without sampling that texture.
    gc::Name getBaseColorTexture(const tg::Material& material)
    {
        const auto& pbr = material.pbrMetallicRoughness;
        const std::array<float, 4> factor{getFactor(pbr.baseColorFactor, 0), getFactor(pbr.baseColorFactor, 1), getFactor(pbr.baseColorFactor, 2),
                                          getFactor(pbr.baseColorFactor, 3)};
        const bool has_factor = factor != std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f};
        const int image_index = getTextureImageIndex(material.name, "base color", pbr.baseColorTexture.index, pbr.baseColorTexture.texCoord);
        if (image_index < 0) {
            return {};
        }

        const std::string description = std::format("base {} {} {} {} {}", image_index, factor[0], factor[1], factor[2], factor[3]);
        return getTexture(description, true, [&]() {
            Pixels pixels = *getImage(image_index);
            if (has_factor) {
                // the factor is a linear color, textures hold sRGB colors
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

        if (mr_image_index < 0 && occlusion_image_index < 0) {
            return {};
        }

        const std::string description = std::format("orm {} {} {} {}", mr_image_index, occlusion_image_index, roughness, metallic);
        return getTexture(description, false, [&]() {
            Pixels pixels{};
            const Pixels* const mr = getImage(mr_image_index);
            const Pixels* const occlusion = getImage(occlusion_image_index);
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

    // empty if the material has no normal map, which makes the engine use the mesh's normals
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

    // Empty if the material has no emissive texture: its emission is then a constant (see getMaterial()).
    // The emissive factor isn't baked into the texture, as it can be more than 1 (with KHR_materials_emissive_strength).
    gc::Name getEmissiveTexture(const tg::Material& material)
    {
        const int image_index = getTextureImageIndex(material.name, "emissive", material.emissiveTexture.index, material.emissiveTexture.texCoord);
        if (image_index < 0) {
            return {};
        }
        return getTexture(std::format("emissive {}", image_index), true, [&]() { return *getImage(image_index); });
    }

    //
    // Materials
    //

    // alphas from here up count as opaque (textures are 8 bit, and compression leaves alphas of 254 in solid areas)
    static constexpr float OPAQUE_ALPHA = 250.0f / 255.0f;
    // alphas between these are neither a hole nor solid
    static constexpr float HOLE_ALPHA = 16.0f / 255.0f;
    // A BLEND material is drawn as a cut out instead if less of its texture than this is partly transparent...
    static constexpr float MAX_PARTIAL_ALPHA_FOR_CUT_OUT = 0.25f;
    // ...or if at least this much of it is holes
    static constexpr float MIN_HOLES_FOR_CUT_OUT = 0.1f;

    struct AlphaStats {
        float min{1.0f};              // the lowest alpha
        float partial_fraction{0.0f}; // how much of it is partly transparent: between HOLE_ALPHA and OPAQUE_ALPHA
        float hole_fraction{0.0f};    // how much of it is (almost) fully transparent: up to HOLE_ALPHA
    };
    std::unordered_map<gc::Name, AlphaStats> m_alpha_stats{}; // of base color textures that have been looked at

    // What the alpha of a material is like: of its base color texture, or of its constant if it doesn't have one
    AlphaStats getAlphaStats(const gc::ResourceMaterial& resource)
    {
        const auto asset_it = m_texture_assets.find(resource.base_color_texture);
        if (asset_it == m_texture_assets.end()) {
            AlphaStats stats{};
            stats.min = resource.base_color.a;
            stats.partial_fraction = (resource.base_color.a > HOLE_ALPHA && resource.base_color.a < OPAQUE_ALPHA) ? 1.0f : 0.0f;
            // (a material that is entirely a hole isn't a cut out: it is nothing. Leave it as the file has it)
            return stats;
        }
        if (const auto it = m_alpha_stats.find(resource.base_color_texture); it != m_alpha_stats.end()) {
            return it->second;
        }

        // the asset is a width, a height, and then RGBA texels
        const std::vector<uint8_t>& data = m_assets[asset_it->second].data;
        const size_t texel_count = (data.size() - 2 * sizeof(uint32_t)) / 4;
        const uint8_t* const texels = data.data() + 2 * sizeof(uint32_t);
        uint8_t min_alpha = 255;
        size_t partial_count = 0;
        size_t hole_count = 0;
        const uint8_t hole = static_cast<uint8_t>(HOLE_ALPHA * 255.0f);
        const uint8_t opaque = static_cast<uint8_t>(OPAQUE_ALPHA * 255.0f);
        for (size_t i = 0; i < texel_count; ++i) {
            const uint8_t alpha = texels[i * 4 + 3];
            min_alpha = std::min(min_alpha, alpha);
            partial_count += (alpha > hole && alpha < opaque) ? 1 : 0;
            hole_count += (alpha <= hole) ? 1 : 0;
        }
        AlphaStats stats{};
        stats.min = static_cast<float>(min_alpha) / 255.0f;
        stats.partial_fraction = (texel_count > 0) ? static_cast<float>(partial_count) / static_cast<float>(texel_count) : 0.0f;
        stats.hole_fraction = (texel_count > 0) ? static_cast<float>(hole_count) / static_cast<float>(texel_count) : 0.0f;
        m_alpha_stats.emplace(resource.base_color_texture, stats);
        return stats;
    }

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


        const auto& pbr = material.pbrMetallicRoughness;
        gc::ResourceMaterial resource{};
        resource.base_color_texture = getBaseColorTexture(material);
        resource.orm_texture = getOrmTexture(material);
        resource.normal_texture = getNormalTexture(material);
        // only used for the textures that the material doesn't have
        resource.base_color = {getFactor(pbr.baseColorFactor, 0), getFactor(pbr.baseColorFactor, 1), getFactor(pbr.baseColorFactor, 2),
                               getFactor(pbr.baseColorFactor, 3)};
        resource.roughness = glm::clamp(static_cast<float>(pbr.roughnessFactor), 0.0f, 1.0f);
        resource.metallic = glm::clamp(static_cast<float>(pbr.metallicFactor), 0.0f, 1.0f);

        // Emission: the emissive texture (or white) times the emissive factor, times the strength if the file has one.
        // A material whose factor is black doesn't glow, whatever its texture is, so it isn't given the texture.
        float emissive_strength = 1.0f;
        if (const auto it = material.extensions.find("KHR_materials_emissive_strength"); it != material.extensions.end()) {
            if (it->second.Has("emissiveStrength") && it->second.Get("emissiveStrength").IsNumber()) {
                emissive_strength = glm::max(static_cast<float>(it->second.Get("emissiveStrength").GetNumberAsDouble()), 0.0f);
            }
        }
        for (int channel = 0; channel < 3; ++channel) {
            const float factor = (static_cast<size_t>(channel) < material.emissiveFactor.size()) ? static_cast<float>(material.emissiveFactor[channel]) : 0.0f;
            resource.emissive[channel] = glm::max(factor, 0.0f) * emissive_strength;
        }
        if (resource.emissive != glm::vec3{0.0f, 0.0f, 0.0f}) {
            resource.emissive_texture = getEmissiveTexture(material);
        }
        // glTF's alpha modes are the engine's blend modes. The alpha is the base color's: the texture's (which has the factor's
        // alpha baked into it), or the factor's if there is no texture.
        if (material.alphaMode == "MASK") {
            resource.blend_mode = gc::MaterialBlendMode::ALPHA_TEST;
            resource.alpha_cutoff = glm::clamp(static_cast<float>(material.alphaCutoff), 0.0f, 1.0f);
        }
        else if (material.alphaMode == "BLEND") {
            resource.blend_mode = gc::MaterialBlendMode::ALPHA_BLEND;
        }
        else if (material.alphaMode != "OPAQUE") {
            GC_WARN("{}: material '{}' uses the unknown alpha mode {}, so it will be opaque", m_name, material.name, material.alphaMode);
        }

        // Don't take the file's word for it. Exporters (Blender, for one) mark a material as BLEND whenever anything is connected
        // to its alpha, even if the texture has no transparency at all, and blended materials are costly: they can't write depth,
        // so they are only sorted by the distance of each object, which looks badly wrong for solid things. So look at the alpha
        // that the material really has, and use the cheapest mode that draws it correctly.
        if (resource.blend_mode != gc::MaterialBlendMode::NONE) {
            const AlphaStats alpha = getAlphaStats(resource);
            const float opaque_above = (resource.blend_mode == gc::MaterialBlendMode::ALPHA_TEST) ? resource.alpha_cutoff : OPAQUE_ALPHA;
            if (alpha.min >= opaque_above) {
                GC_DEBUG("{}: material '{}' is {} but has no transparency, so it will be opaque", m_name, material.name, material.alphaMode);
                resource.blend_mode = gc::MaterialBlendMode::NONE;
            }
            else if (resource.blend_mode == gc::MaterialBlendMode::ALPHA_BLEND &&
                     (alpha.partial_fraction < MAX_PARTIAL_ALPHA_FOR_CUT_OUT || alpha.hole_fraction >= MIN_HOLES_FOR_CUT_OUT)) {
                // Nearly all of it is either solid or a hole, as fences are, or a lot of it is holes, as leaves are (their
                // textures are mostly empty, with soft edges). Something see-through, like glass, has neither.
                GC_INFO("{}: material '{}' is BLEND but its alpha is a cut out ({:.0f}% holes, {:.0f}% partly transparent), so it will be alpha tested",
                        m_name, material.name, alpha.hole_fraction * 100.0f, alpha.partial_fraction * 100.0f);
                resource.blend_mode = gc::MaterialBlendMode::ALPHA_TEST;
                resource.alpha_cutoff = 0.5f;
            }
        }

        const std::string asset_name = (material_index >= 0) ? std::format("{}/material{}", m_name, material_index) : std::format("{}/material_default", m_name);
        m_assets.push_back(makeAsset(asset_name, makeMaterialData(resource), gcpak::GcpakAssetType::MATERIAL));
        const gc::Name name(asset_name);
        m_material_resources.emplace(name, resource);
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
            m_mesh_assets.emplace(gc::Name(part_name), m_assets.size());
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

    // If the scene has a directional light, bakes the shadows that the scene's meshes cast in it. The shadow map becomes an asset,
    // and the prefab gets another root entity with a ShadowMapComponent that uses it. The engine draws those shadows wherever the
    // prefab is put, as long as the light isn't turned.
    void bakeShadows()
    {
        if (!m_direction_to_light || m_shadow_casters.empty()) {
            return;
        }

        gc::Stopwatch stopwatch{};
        std::vector<ShadowCaster> casters{};
        casters.reserve(m_shadow_casters.size());
        for (const auto& [draw, matrix] : m_shadow_casters) {
            ShadowCaster caster{};
            caster.mesh_data = m_assets[m_mesh_assets.at(draw.mesh)].data;
            caster.matrix = matrix;

            // The holes of alpha tested materials don't cast shadows. Alpha blended materials can't cast partial shadows, as a
            // shadow map only has one depth, so they cast a full shadow where they are mostly opaque and none elsewhere.
            const gc::ResourceMaterial& material = m_material_resources.at(draw.material);
            if (material.blend_mode != gc::MaterialBlendMode::NONE) {
                caster.alpha_cutoff = (material.blend_mode == gc::MaterialBlendMode::ALPHA_TEST) ? material.alpha_cutoff : 0.5f;
                caster.alpha = material.base_color.a;
                if (const auto it = m_texture_assets.find(material.base_color_texture); it != m_texture_assets.end()) {
                    caster.alpha_texture_data = m_assets[it->second].data;
                }
            }
            casters.push_back(caster);
        }
        const std::optional<BakedShadowMap> shadow_map = bakeShadowMap(casters, *m_direction_to_light);
        if (!shadow_map) {
            return;
        }

        const std::string asset_name = std::format("{}/shadowmap", m_name);
        m_assets.push_back(makeAsset(asset_name, makeShadowMapData(*shadow_map), gcpak::GcpakAssetType::SHADOW_MAP_R16));

        // Surfaces are moved off themselves by a couple of texels before they are looked up, and a little towards the light: by
        // the depth that a texel covers on a surface at 45 degrees to the light, plus what is lost by storing depths in 16 bits.
        const float normal_bias = 2.0f * shadow_map->texel_size;
        const float depth_bias = shadow_map->texel_size / shadow_map->depth_range + 4.0f / 65535.0f;

        // A root with no transform of its own, so the shadow map's matrix is from prefab space
        m_prefab.beginEntity(gc::Name("shadow_map"));
        m_entity_matrices.push_back(glm::mat4{1.0f});
        m_prefab.addComponent(gc::ShadowMapComponent{}.setShadowMap(gc::Name(asset_name)).setMatrix(shadow_map->matrix).setBias(normal_bias, depth_bias));

        GC_INFO("{}: baked a {}x{} shadow map of {} triangles in {}. Its texels are {:.3f} m wide", m_name, shadow_map->resolution,
                shadow_map->resolution, shadow_map->triangle_count, stopwatch, shadow_map->texel_size);
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

        // where the entity is in the prefab, for baking shadows
        glm::mat4 local_matrix = glm::mat4_cast(rotation);
        local_matrix[0] *= scale.x;
        local_matrix[1] *= scale.y;
        local_matrix[2] *= scale.z;
        local_matrix[3] = glm::vec4(position, 1.0f);
        const glm::mat4 entity_matrix = m_entity_matrices[parent] * local_matrix;
        m_entity_matrices.push_back(entity_matrix);
        for (const Draw& draw : draws) {
            m_shadow_casters.emplace_back(draw, entity_matrix);
        }

        if (draws.size() == 1) {
            m_prefab.addComponent(gc::RenderableComponent{}.setMesh(draws[0].mesh).setMaterial(draws[0].material));
            // what is drawn is also what is solid
            m_prefab.addComponent(gc::ColliderComponent{}.setMesh(draws[0].mesh));
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

        if (node.light >= 0 && node.light < static_cast<int>(m_model.lights.size())) {
            const tg::Light& light = m_model.lights[node.light];
            // glTF lights shine along their -Z axis, as the engine's do.
            gc::LightComponent component{};
            if (light.type == "directional") {
                component.setType(gc::LightType::DIRECTIONAL);
                // The light's +Z axis points towards it. If there are several directional lights, the engine uses the last one.
                const glm::vec3 z_axis{entity_matrix[2]};
                if (glm::dot(z_axis, z_axis) > 0.0f) {
                    m_direction_to_light = glm::normalize(z_axis);
                }
            }
            else {
                component.setType(gc::LightType::POINT);
                if (light.type != "point") {
                    GC_WARN("{}: the light of node '{}' is a {} light, which isn't supported, so it becomes a point light", m_name, node_name, light.type);
                }
            }
            component.setColor({getFactor(light.color, 0), getFactor(light.color, 1), getFactor(light.color, 2)});
            // The engine uses glTF's units: lux for directional lights and candela for point lights. As the glTF specification
            // says, the scale of the node (and of its parents) doesn't change how bright the light is.
            component.setIntensity(glm::max(static_cast<float>(light.intensity), 0.0f));
            // a glTF light without a range has no limit, which is what zero means to the engine
            component.setRange(light.range > 0.0 ? static_cast<float>(light.range) : 0.0f);
            m_prefab.addComponent(component);
        }

        if (draws.size() > 1) {
            // an entity can only have one renderable
            for (size_t i = 0; i < draws.size(); ++i) {
                m_prefab.beginEntity(gc::Name(std::format("{}_mesh{}", node_name, i)), entity);
                m_entity_matrices.push_back(entity_matrix);
                m_prefab.addComponent(gc::RenderableComponent{}.setMesh(draws[i].mesh).setMaterial(draws[i].material));
                m_prefab.addComponent(gc::ColliderComponent{}.setMesh(draws[i].mesh));
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
