//
// ember_court_mapgen.exe
//
// Makes Ember Court's map: content/ember_court.gcpak.
//
//     ember_court_mapgen OUTPUT.gcpak
//
// The whole map is one prefab ("ember_court.map"), built here with gc::PrefabWriter rather than exported from a modelling program:
// the ground, walls and buildings, their materials, the sun, sky and lamps, a camera, and the places that the game's rules need to
// know about (spawn points, pickups, patrol routes: see components.h). The meshes, materials and textures that the prefab uses
// are assets in the same file. Nothing here needs a window or a renderer.
//
// The engine only draws shadows that were baked beforehand, so the map isn't finished until gcpak_editor has baked them:
//
//     gcpak_editor --bake-shadows ember_court.map OUTPUT.gcpak --no-cast sky --no-cast beacon
//
// The build does both (see CMakeLists.txt).
//

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <array>
#include <filesystem>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <gcpak/gcpak.h>

#include <gamecore/gc_asset_data.h>
#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_collider_component.h>
#include <gamecore/gc_gen_tangents.h>
#include <gamecore/gc_light_component.h>
#include <gamecore/gc_mesh_vertex.h>
#include <gamecore/gc_prefab.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_resources.h>

#include "components.h"

using Asset = gcpak::GcpakCreator::Asset;

namespace {

constexpr float COURT_HALF_SIZE = 24.0f; // the court is this far across in each direction from the middle, inside its walls

// Lights are in physical units. It is evening: the sun is low and dim enough (lux) that the lamps (candela) can be seen next to
// it. Cameras need an exposure that suits it: see CAMERA_EXPOSURE_EV100 in match.h.
constexpr float SUN_ILLUMINANCE = 700.0f;
constexpr float SKY_ILLUMINANCE = 220.0f;
constexpr float CAMERA_EXPOSURE = 7.5f;
const glm::vec3 DIRECTION_TO_SUN = glm::normalize(glm::vec3{-0.45f, -0.55f, 0.6f}); // it is in the south west

const glm::quat NO_ROTATION{1.0f, 0.0f, 0.0f, 0.0f};

glm::quat turnedAboutZ(float degrees) { return glm::angleAxis(glm::radians(degrees), glm::vec3{0.0f, 0.0f, 1.0f}); }

//
// Meshes
//

// Removes duplicate vertices, makes the tangents, and packs the result as a mesh asset
std::vector<uint8_t> finishMesh(std::vector<gc::MeshVertex> vertices)
{
    const std::vector<int> remap = gc::genTangents(vertices);
    std::vector<uint16_t> indices{};
    indices.reserve(remap.size());
    for (const int index : remap) {
        indices.push_back(static_cast<uint16_t>(index));
    }
    return gc::makeMeshData(vertices, indices);
}

// A box with its centre at the origin. Its texture repeats every 'tile' metres, the right way up on the sides.
std::vector<uint8_t> makeBoxMesh(const glm::vec3& size, float tile)
{
    struct Face {
        glm::vec3 normal;
        glm::vec3 u; // the directions that the texture coordinates increase in. u x v = normal
        glm::vec3 v;
    };
    static const std::array<Face, 6> FACES{{
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {1, 0, 0}, {0, -1, 0}},
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
        {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}},
        {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
    }};

    const glm::vec3 half = 0.5f * size;
    std::vector<gc::MeshVertex> vertices{};
    vertices.reserve(36);
    for (const Face& face : FACES) {
        const float half_u = glm::dot(glm::abs(face.u), half);
        const float half_v = glm::dot(glm::abs(face.v), half);
        const glm::vec3 centre = face.normal * glm::dot(glm::abs(face.normal), half);
        const auto corner = [&](float su, float sv) {
            gc::MeshVertex vertex{};
            vertex.position = centre + face.u * (su * half_u) + face.v * (sv * half_v);
            vertex.normal = face.normal;
            vertex.uv = glm::vec2{(su + 1.0f) * half_u, (sv + 1.0f) * half_v} / tile;
            return vertex;
        };
        // two triangles, anticlockwise when seen from outside
        vertices.push_back(corner(-1, -1));
        vertices.push_back(corner(1, -1));
        vertices.push_back(corner(-1, 1));
        vertices.push_back(corner(-1, 1));
        vertices.push_back(corner(1, -1));
        vertices.push_back(corner(1, 1));
    }
    return finishMesh(std::move(vertices));
}

// A sphere with a radius of 1. Its texture wraps round it once, with the bottom of the texture at the bottom (-Z) of the sphere.
// 'inside_out' makes it visible from the inside instead, for the sky.
std::vector<uint8_t> makeSphereMesh(int rings, int segments, bool inside_out)
{
    const auto point = [&](int ring, int segment) {
        const float theta = glm::pi<float>() * static_cast<float>(ring) / static_cast<float>(rings); // from the top
        const float phi = glm::two_pi<float>() * static_cast<float>(segment) / static_cast<float>(segments);
        gc::MeshVertex vertex{};
        vertex.position = glm::vec3{std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)};
        vertex.normal = vertex.position;
        vertex.uv = glm::vec2{static_cast<float>(segment) / static_cast<float>(segments), 1.0f - static_cast<float>(ring) / static_cast<float>(rings)};
        return vertex;
    };

    std::vector<gc::MeshVertex> vertices{};
    for (int ring = 0; ring < rings; ++ring) {
        for (int segment = 0; segment < segments; ++segment) {
            const std::array<gc::MeshVertex, 6> quad{point(ring, segment),         point(ring + 1, segment), point(ring + 1, segment + 1),
                                                     point(ring + 1, segment + 1), point(ring, segment + 1), point(ring, segment)};
            for (int triangle = 0; triangle < 2; ++triangle) {
                // (the triangles at the poles have two corners in the same place, which is harmless)
                if (inside_out) {
                    vertices.push_back(quad[triangle * 3 + 2]);
                    vertices.push_back(quad[triangle * 3 + 1]);
                    vertices.push_back(quad[triangle * 3]);
                }
                else {
                    vertices.push_back(quad[triangle * 3]);
                    vertices.push_back(quad[triangle * 3 + 1]);
                    vertices.push_back(quad[triangle * 3 + 2]);
                }
            }
        }
    }
    return finishMesh(std::move(vertices));
}

//
// Textures
//

uint8_t toSrgb(float linear)
{
    linear = glm::clamp(linear, 0.0f, 1.0f);
    const float srgb = (linear <= 0.0031308f) ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::lround(srgb * 255.0f));
}

// The evening sky, as seen from the middle of the map: one texel for every direction, wrapped round the sky's sphere.
// It glows where the sun is, so that what casts the shadows can be seen.
std::vector<uint8_t> makeSkyTexture()
{
    constexpr uint32_t WIDTH = 512;
    constexpr uint32_t HEIGHT = 256;
    const glm::vec3 horizon{0.95f, 0.5f, 0.24f};
    const glm::vec3 zenith{0.07f, 0.16f, 0.42f};
    const glm::vec3 below{0.05f, 0.055f, 0.07f};
    const glm::vec3 sun_color{1.0f, 0.8f, 0.5f};

    std::vector<uint8_t> rgba(static_cast<size_t>(WIDTH) * HEIGHT * 4);
    for (uint32_t y = 0; y < HEIGHT; ++y) {
        // the first row of a texture is its bottom
        const float elevation = ((static_cast<float>(y) + 0.5f) / static_cast<float>(HEIGHT) - 0.5f) * glm::pi<float>();
        for (uint32_t x = 0; x < WIDTH; ++x) {
            const float phi = (static_cast<float>(x) + 0.5f) / static_cast<float>(WIDTH) * glm::two_pi<float>();
            const glm::vec3 direction{std::cos(elevation) * std::cos(phi), std::cos(elevation) * std::sin(phi), std::sin(elevation)};

            glm::vec3 color{};
            if (direction.z >= 0.0f) {
                color = glm::mix(horizon, zenith, std::pow(direction.z, 0.45f));
            }
            else {
                color = glm::mix(horizon * 0.6f, below, glm::clamp(-direction.z * 6.0f, 0.0f, 1.0f));
            }
            const float towards_sun = glm::max(glm::dot(direction, DIRECTION_TO_SUN), 0.0f);
            color += sun_color * (2.0f * std::pow(towards_sun, 600.0f) + 0.45f * std::pow(towards_sun, 10.0f));

            uint8_t* const texel = &rgba[(static_cast<size_t>(y) * WIDTH + x) * 4];
            texel[0] = toSrgb(color.r);
            texel[1] = toSrgb(color.g);
            texel[2] = toSrgb(color.b);
            texel[3] = 255;
        }
    }
    return gc::makeTextureData(WIDTH, HEIGHT, rgba);
}

// An iron grating: bars with holes between them. The holes are in the alpha, for an alpha tested material.
std::vector<uint8_t> makeGrateTexture()
{
    constexpr uint32_t SIZE = 128;
    constexpr uint32_t SPACING = 32; // four bars across the texture
    constexpr uint32_t BAR = 7;
    std::vector<uint8_t> rgba(static_cast<size_t>(SIZE) * SIZE * 4);
    for (uint32_t y = 0; y < SIZE; ++y) {
        for (uint32_t x = 0; x < SIZE; ++x) {
            const bool bar = (x % SPACING) < BAR || (y % SPACING) < BAR;
            uint8_t* const texel = &rgba[(static_cast<size_t>(y) * SIZE + x) * 4];
            texel[0] = toSrgb(0.08f);
            texel[1] = toSrgb(0.075f);
            texel[2] = toSrgb(0.07f);
            texel[3] = bar ? 255 : 0;
        }
    }
    return gc::makeTextureData(SIZE, SIZE, rgba);
}

//
// The map
//

class MapBuilder {
    std::vector<Asset> m_assets{};
    gc::PrefabWriter m_prefab{};
    std::unordered_map<std::string, gc::Name> m_box_meshes{}; // boxes of the same size share a mesh

public:
    gc::PrefabWriter& prefab() { return m_prefab; }
    const std::vector<Asset>& getAssets() const { return m_assets; }

    // Assets are found by the hash of their name, which is what components store
    gc::Name addAsset(const std::string& name, std::vector<uint8_t> data, gcpak::GcpakAssetType type)
    {
        Asset asset{};
        asset.name = name;
        asset.data = std::move(data);
        asset.type = type;
        m_assets.push_back(std::move(asset));
        return gc::Name(name);
    }

    gc::Name addMaterial(const std::string& name, const gc::ResourceMaterial& material)
    {
        return addAsset("ember_court/" + name, gc::makeMaterialData(material), gcpak::GcpakAssetType::MATERIAL);
    }

    // An entity with nothing on it, to keep the entities under it together
    uint32_t addGroup(const std::string& name, uint32_t parent = gcpak::PREFAB_NO_PARENT) { return m_prefab.beginEntity(gc::Name(name), parent); }

    // A box that is drawn and, unless it says otherwise, solid. The same mesh is what is drawn, what things collide with, and
    // what gcpak_editor bakes the shadow of.
    uint32_t addBox(uint32_t parent, const std::string& name, const glm::vec3& centre, const glm::vec3& size, gc::Name material, float tile = 2.0f,
                    const glm::quat& rotation = NO_ROTATION, bool solid = true)
    {
        const std::string mesh_name = std::format("ember_court/box_{:g}x{:g}x{:g}_{:g}", size.x, size.y, size.z, tile);
        auto it = m_box_meshes.find(mesh_name);
        if (it == m_box_meshes.end()) {
            const gc::Name mesh = addAsset(mesh_name, makeBoxMesh(size, tile), gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16);
            it = m_box_meshes.emplace(mesh_name, mesh).first;
        }
        const uint32_t entity = m_prefab.beginEntity(gc::Name(name), parent, centre, rotation);
        m_prefab.addComponent(gc::RenderableComponent{}.setMesh(it->second).setMaterial(material));
        if (solid) {
            m_prefab.addComponent(gc::ColliderComponent{}.setMesh(it->second));
        }
        return entity;
    }

    // A slope to walk up, from the middle of its bottom edge to the middle of its top edge
    uint32_t addRamp(uint32_t parent, const std::string& name, const glm::vec3& bottom, const glm::vec3& top, float width, gc::Name material)
    {
        constexpr float THICKNESS = 0.3f;
        const glm::vec3 along = top - bottom;
        const float length = glm::length(along);
        // the box's X axis is turned to point up the slope
        const glm::quat rotation = glm::angleAxis(std::atan2(along.y, along.x), glm::vec3{0.0f, 0.0f, 1.0f}) *
                                   glm::angleAxis(-std::asin(along.z / length), glm::vec3{0.0f, 1.0f, 0.0f});
        const glm::vec3 normal = rotation * glm::vec3{0.0f, 0.0f, 1.0f};
        // (a little longer than the slope, so that its ends go into the ground and the platform rather than stopping short)
        return addBox(parent, name, 0.5f * (bottom + top) - normal * (0.5f * THICKNESS), {length + 0.3f, width, THICKNESS}, material, 2.0f, rotation);
    }

    // A lamp: something that glows, with a point light just under it. Emissive materials don't light anything by themselves.
    void addLamp(uint32_t parent, const std::string& name, const glm::vec3& position, gc::Name material, const glm::vec3& color, float candela, float range)
    {
        addBox(parent, name, position, {0.3f, 0.3f, 0.4f}, material, 1.0f, NO_ROTATION, false);
        m_prefab.beginEntity(gc::Name(name + "_light"), parent, position - glm::vec3{0.0f, 0.0f, 0.4f});
        m_prefab.addComponent(gc::LightComponent{}.setType(gc::LightType::POINT).setColor(color).setIntensity(candela).setRange(range));
    }
};

gc::ResourceMaterial makeTexturedMaterial(const char* base_color, const char* orm, const char* normal)
{
    // Materials only name their textures. These ones are in content/textures.gcpak: it doesn't matter which file an asset is in.
    gc::ResourceMaterial material{};
    material.base_color_texture = base_color ? gc::Name(base_color) : gc::Name{};
    material.orm_texture = orm ? gc::Name(orm) : gc::Name{};
    material.normal_texture = normal ? gc::Name(normal) : gc::Name{};
    material.roughness = 0.9f; // (for the ones without an occlusion-roughness-metallic texture)
    return material;
}

gc::ResourceMaterial makePlainMaterial(const glm::vec3& color, float roughness, float metallic = 0.0f)
{
    gc::ResourceMaterial material{};
    material.base_color = glm::vec4{color, 1.0f};
    material.roughness = roughness;
    material.metallic = metallic;
    return material;
}

gc::ResourceMaterial makeGlowingMaterial(const glm::vec3& emissive)
{
    gc::ResourceMaterial material = makePlainMaterial({0.02f, 0.02f, 0.02f}, 1.0f);
    material.emissive = emissive;
    return material;
}

std::vector<Asset> buildMap()
{
    MapBuilder map{};
    gc::PrefabWriter& prefab = map.prefab();

    //
    // Materials. Between them they use every kind that the renderer has: with all the textures, with only a color texture, with
    // none at all, see-through (alpha blended), cut out (alpha tested), and glowing (emissive).
    //
    const gc::Name paving = map.addMaterial("paving", makeTexturedMaterial("bricks-mortar-albedo.png", "bricks-mortar-orm.png", "bricks-mortar-normal.png"));
    const gc::Name wood = map.addMaterial(
        "wood", makeTexturedMaterial("laminate-flooring-brown_albedo.png", "laminate-flooring-brown_orm.png", "laminate-flooring-brown_normal.png"));
    const gc::Name brick = map.addMaterial("brick", makeTexturedMaterial("bricks.jpg", nullptr, nullptr));
    const gc::Name crate = map.addMaterial("crate", makeTexturedMaterial("box.jpg", nullptr, nullptr));
    const gc::Name stone = map.addMaterial("stone", makePlainMaterial({0.56f, 0.54f, 0.5f}, 0.9f));
    const gc::Name dark_stone = map.addMaterial("dark_stone", makePlainMaterial({0.16f, 0.16f, 0.18f}, 0.35f));
    const gc::Name iron = map.addMaterial("iron", makePlainMaterial({0.45f, 0.45f, 0.47f}, 0.4f, 1.0f));
    gc::Name glass{};
    {
        gc::ResourceMaterial material = makePlainMaterial({0.55f, 0.8f, 0.95f}, 0.05f);
        material.base_color.a = 0.3f;
        material.blend_mode = gc::MaterialBlendMode::ALPHA_BLEND;
        glass = map.addMaterial("glass", material);
    }
    gc::Name grate{};
    {
        const gc::Name texture = map.addAsset("ember_court/grate.png", makeGrateTexture(), gcpak::GcpakAssetType::TEXTURE_R8G8B8A8_SRGB);
        gc::ResourceMaterial material = makePlainMaterial({1.0f, 1.0f, 1.0f}, 0.5f, 1.0f);
        material.base_color_texture = texture;
        material.blend_mode = gc::MaterialBlendMode::ALPHA_TEST;
        material.alpha_cutoff = 0.5f;
        grate = map.addMaterial("grate", material);
    }
    const gc::Name fire = map.addMaterial("fire", makeGlowingMaterial({1.0f, 0.42f, 0.08f}));
    const gc::Name warm_lamp = map.addMaterial("warm_lamp", makeGlowingMaterial({1.0f, 0.75f, 0.4f}));
    const gc::Name cold_lamp = map.addMaterial("cold_lamp", makeGlowingMaterial({0.5f, 0.75f, 1.0f}));
    const gc::Name beacon = map.addMaterial("beacon", makeGlowingMaterial({0.2f, 0.95f, 0.85f}));
    gc::Name sky{};
    {
        gc::ResourceMaterial material = makePlainMaterial({0.0f, 0.0f, 0.0f}, 1.0f); // it only glows
        material.emissive_texture = map.addAsset("ember_court/sky.png", makeSkyTexture(), gcpak::GcpakAssetType::TEXTURE_R8G8B8A8_SRGB);
        material.emissive = glm::vec3{1.0f, 1.0f, 1.0f};
        sky = map.addMaterial("sky", material);
    }

    const gc::Name sphere_mesh =
        map.addAsset("ember_court/sphere", makeSphereMesh(16, 32, false), gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16);
    const gc::Name sky_mesh =
        map.addAsset("ember_court/sky_sphere", makeSphereMesh(24, 48, true), gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16);

    //
    // The court: the ground and the walls around it
    //
    const float half = COURT_HALF_SIZE;
    const uint32_t court = map.addGroup("court");
    map.addBox(court, "ground", {0.0f, 0.0f, -0.5f}, {2.0f * half + 12.0f, 2.0f * half + 12.0f, 1.0f}, paving, 3.0f);
    constexpr float WALL_HEIGHT = 4.5f;
    map.addBox(court, "wall_north", {0.0f, half + 0.5f, 0.5f * WALL_HEIGHT}, {2.0f * half + 2.0f, 1.0f, WALL_HEIGHT}, brick, 2.5f);
    map.addBox(court, "wall_south", {0.0f, -half - 0.5f, 0.5f * WALL_HEIGHT}, {2.0f * half + 2.0f, 1.0f, WALL_HEIGHT}, brick, 2.5f);
    map.addBox(court, "wall_east", {half + 0.5f, 0.0f, 0.5f * WALL_HEIGHT}, {1.0f, 2.0f * half, WALL_HEIGHT}, brick, 2.5f);
    map.addBox(court, "wall_west", {-half - 0.5f, 0.0f, 0.5f * WALL_HEIGHT}, {1.0f, 2.0f * half, WALL_HEIGHT}, brick, 2.5f);

    //
    // The keep, in the middle: a raised floor with a ramp on either side, under a roof on four pillars. A fire burns on it.
    //
    {
        const uint32_t keep = map.addGroup("keep");
        constexpr float FLOOR = 1.5f;
        map.addBox(keep, "keep_floor", {0.0f, 0.0f, 0.5f * FLOOR}, {10.0f, 10.0f, FLOOR}, stone);
        map.addRamp(keep, "keep_ramp_east", {10.5f, 0.0f, 0.0f}, {5.0f, 0.0f, FLOOR}, 3.0f, wood);
        map.addRamp(keep, "keep_ramp_west", {-10.5f, 0.0f, 0.0f}, {-5.0f, 0.0f, FLOOR}, 3.0f, wood);
        for (const float x : {-4.4f, 4.4f}) {
            for (const float y : {-4.4f, 4.4f}) {
                map.addBox(keep, "keep_pillar", {x, y, FLOOR + 1.75f}, {0.7f, 0.7f, 3.5f}, stone);
            }
        }
        map.addBox(keep, "keep_roof", {0.0f, 0.0f, FLOOR + 3.7f}, {11.0f, 11.0f, 0.4f}, wood);

        map.addBox(keep, "brazier", {0.0f, 0.0f, FLOOR + 0.35f}, {0.9f, 0.9f, 0.7f}, iron, 1.0f);
        prefab.beginEntity(gc::Name("brazier_fire"), keep, {0.0f, 0.0f, FLOOR + 0.8f}, NO_ROTATION, glm::vec3{0.32f});
        prefab.addComponent(gc::RenderableComponent{}.setMesh(sphere_mesh).setMaterial(fire));
        prefab.beginEntity(gc::Name("brazier_light"), keep, {0.0f, 0.0f, FLOOR + 1.5f});
        prefab.addComponent(gc::LightComponent{}.setType(gc::LightType::POINT).setColor({1.0f, 0.55f, 0.25f}).setIntensity(2200.0f).setRange(13.0f));

        // On top of the roof, something that turns. The game's SpinComponent is stored in the prefab like any other component.
        // (It isn't solid and has to be left out of the shadow bake, as neither a collider nor a baked shadow can turn with it.)
        prefab.beginEntity(gc::Name("beacon"), keep, {0.0f, 0.0f, FLOOR + 5.0f});
        SpinComponent spin{};
        spin.radians_per_second = 0.9f;
        prefab.addComponent(spin);
        map.addBox(prefab.getEntityCount() - 1, "beacon_crystal", {0.0f, 0.0f, 0.0f}, {0.9f, 0.9f, 0.9f}, beacon, 1.0f,
                   glm::angleAxis(glm::radians(45.0f), glm::normalize(glm::vec3{1.0f, 1.0f, 0.0f})), false);
    }

    //
    // The arcade, along the north wall: a roof on a row of pillars, with lamps under it. The sun shines in between the pillars.
    //
    {
        const uint32_t arcade = map.addGroup("arcade");
        constexpr float DEPTH = 6.0f;
        constexpr float HEIGHT = 3.5f;
        map.addBox(arcade, "arcade_roof", {0.0f, half - 0.5f * DEPTH, HEIGHT + 0.15f}, {40.0f, DEPTH, 0.3f}, wood);
        constexpr int PILLARS = 8; // (the same mesh and material this many times is enough for the renderer to instance them)
        for (int i = 0; i < PILLARS; ++i) {
            const float x = -19.5f + 39.0f * static_cast<float>(i) / static_cast<float>(PILLARS - 1);
            map.addBox(arcade, "arcade_pillar", {x, half - DEPTH + 0.4f, 0.5f * HEIGHT}, {0.6f, 0.6f, HEIGHT}, stone);
        }
        for (const float x : {-13.0f, 0.0f, 13.0f}) {
            map.addLamp(arcade, "arcade_lamp", {x, half - 0.5f * DEPTH, HEIGHT - 0.25f}, warm_lamp, {1.0f, 0.72f, 0.42f}, 1100.0f, 10.0f);
        }
    }

    //
    // The screen, in the west: iron posts with panels of glass and grating between them. Glass is see-through and casts no
    // shadow. The grating is a flat panel with holes cut out of it by its texture, and so is its shadow.
    //
    {
        const uint32_t screen = map.addGroup("screen");
        constexpr float X = -14.0f;
        constexpr float FIRST_Y = -12.0f;
        constexpr float SPACING = 3.0f;
        constexpr int PANELS = 6;
        constexpr float HEIGHT = 3.0f;
        for (int i = 0; i <= PANELS; ++i) {
            map.addBox(screen, "screen_post", {X, FIRST_Y + SPACING * static_cast<float>(i), 0.5f * HEIGHT}, {0.35f, 0.35f, HEIGHT}, iron, 1.0f);
        }
        map.addBox(screen, "screen_rail", {X, FIRST_Y + 0.5f * SPACING * static_cast<float>(PANELS), HEIGHT + 0.1f},
                   {0.4f, SPACING * static_cast<float>(PANELS) + 0.4f, 0.2f}, iron, 1.0f);
        for (int i = 0; i < PANELS; ++i) {
            const bool is_glass = (i % 2) == 0;
            map.addBox(screen, is_glass ? "screen_glass" : "screen_grate", {X, FIRST_Y + SPACING * (static_cast<float>(i) + 0.5f), 0.5f * HEIGHT},
                       {0.06f, SPACING - 0.35f, HEIGHT}, is_glass ? glass : grate, 1.0f);
        }
    }

    //
    // The tower, in the south east corner: a platform on posts, with a long ramp up to it and a lamp under it
    //
    {
        const uint32_t tower = map.addGroup("tower");
        constexpr float TOP = 3.2f;
        const glm::vec3 centre{19.0f, -19.0f, 0.0f};
        map.addBox(tower, "tower_floor", centre + glm::vec3{0.0f, 0.0f, TOP - 0.2f}, {8.0f, 8.0f, 0.4f}, wood);
        for (const float x : {-3.6f, 3.6f}) {
            for (const float y : {-3.6f, 3.6f}) {
                map.addBox(tower, "tower_post", centre + glm::vec3{x, y, 0.5f * (TOP - 0.4f)}, {0.5f, 0.5f, TOP - 0.4f}, stone);
            }
        }
        map.addRamp(tower, "tower_ramp", {19.0f, -3.5f, 0.0f}, {19.0f, -15.0f, TOP}, 2.6f, wood);
        // low walls on the two sides that face the court, to hide behind
        map.addBox(tower, "tower_wall_west", centre + glm::vec3{-3.85f, 0.0f, TOP + 0.5f}, {0.3f, 8.0f, 1.0f}, stone);
        map.addBox(tower, "tower_wall_north", centre + glm::vec3{-2.6f, 3.85f, TOP + 0.5f}, {2.8f, 0.3f, 1.0f}, stone);
        map.addLamp(tower, "tower_lamp", centre + glm::vec3{0.0f, 0.0f, TOP - 0.65f}, cold_lamp, {0.55f, 0.75f, 1.0f}, 900.0f, 9.0f);
    }

    //
    // Things to hide behind and climb on
    //
    {
        const uint32_t cover = map.addGroup("cover");
        // the monolith, in the corner that the sun is in, casts the longest shadow in the court
        map.addBox(cover, "monolith", {-17.5f, -17.5f, 3.5f}, {2.6f, 2.6f, 7.0f}, dark_stone, 2.0f, turnedAboutZ(30.0f));

        map.addBox(cover, "low_wall", {-6.0f, -14.0f, 0.6f}, {5.0f, 0.5f, 1.2f}, brick, 2.5f);
        map.addBox(cover, "low_wall", {7.0f, -13.0f, 0.6f}, {0.5f, 4.0f, 1.2f}, brick, 2.5f);
        map.addBox(cover, "low_wall", {-7.0f, 12.5f, 0.6f}, {4.0f, 0.5f, 1.2f}, brick, 2.5f);
        map.addBox(cover, "low_wall", {-19.0f, 9.0f, 0.6f}, {0.5f, 5.0f, 1.2f}, brick, 2.5f);

        struct Crate {
            glm::vec3 position; // of the middle of its bottom
            float size;
            float turn; // degrees
        };
        // (a crate's texture covers each side once, so its mesh is tiled by its own size)
        for (const Crate& c : std::array<Crate, 9>{{
                 {{14.0f, -6.0f, 0.0f}, 1.0f, 0.0f},
                 {{15.3f, -6.4f, 0.0f}, 1.0f, 20.0f},
                 {{14.4f, -4.6f, 0.0f}, 1.5f, -15.0f},
                 {{14.6f, -6.2f, 1.0f}, 1.0f, 35.0f},
                 {{10.0f, 12.0f, 0.0f}, 1.5f, 10.0f},
                 {{11.8f, 12.6f, 0.0f}, 1.0f, -25.0f},
                 {{10.2f, 12.1f, 1.5f}, 1.0f, 50.0f},
                 {{-3.0f, -20.0f, 0.0f}, 1.0f, 8.0f},
                 {{5.5f, 20.0f, 0.0f}, 1.5f, -5.0f},
             }}) {
            map.addBox(cover, "crate", c.position + glm::vec3{0.0f, 0.0f, 0.5f * c.size}, glm::vec3{c.size}, crate, c.size, turnedAboutZ(c.turn));
        }
    }

    //
    // The sky and the lights
    //
    {
        // A sphere far outside the map, seen from the inside. It is named so that the shadow bake can be told to leave it out
        // (--no-cast sky): it is between the sun and everything else.
        prefab.beginEntity(gc::Name("sky"), gcpak::PREFAB_NO_PARENT, {0.0f, 0.0f, 0.0f}, NO_ROTATION, glm::vec3{400.0f});
        prefab.addComponent(gc::RenderableComponent{}.setMesh(sky_mesh).setMaterial(sky));

        // Lights shine along their -Z axis, as cameras look. This is the light that gcpak_editor bakes the shadows of.
        prefab.beginEntity(gc::Name("sun"), gcpak::PREFAB_NO_PARENT, {0.0f, 0.0f, 0.0f}, glm::quatLookAt(-DIRECTION_TO_SUN, glm::vec3{0.0f, 0.0f, 1.0f}));
        prefab.addComponent(gc::LightComponent{}.setType(gc::LightType::DIRECTIONAL).setColor({1.0f, 0.82f, 0.62f}).setIntensity(SUN_ILLUMINANCE));

        // what the sky lights the shadows with
        prefab.beginEntity(gc::Name("sky_light"));
        prefab.addComponent(gc::LightComponent{}.setType(gc::LightType::AMBIENT).setColor({0.55f, 0.7f, 1.0f}).setIntensity(SKY_ILLUMINANCE));
    }

    //
    // A camera that looks over the court, for when there is no player to look through. Cameras in prefabs aren't active.
    //
    {
        const glm::vec3 position{-21.0f, -21.5f, 13.0f};
        const glm::vec3 looking_at{3.0f, 4.0f, 0.0f};
        prefab.beginEntity(gc::Name("overview_camera"), gcpak::PREFAB_NO_PARENT, position,
                           glm::quatLookAt(glm::normalize(looking_at - position), glm::vec3{0.0f, 0.0f, 1.0f}));
        prefab.addComponent(gc::CameraComponent{}.setFOV(glm::radians(60.0f)).setNearPlane(0.1f).setActive(false).setExposure(CAMERA_EXPOSURE));
    }

    //
    // What the game's rules need to know. These are the game's own components (components.h): the engine knows nothing about
    // them, and stores them in the prefab by name like any others.
    //
    {
        const uint32_t gameplay = map.addGroup("gameplay");

        // players appear facing the middle of the court
        for (const glm::vec2& p : std::array<glm::vec2, 8>{{
                 {-20.0f, 15.0f},
                 {20.0f, 15.0f},
                 {-10.0f, -21.0f},
                 {9.0f, -21.0f},
                 {21.0f, 3.0f},
                 {-21.0f, -4.0f},
                 {0.0f, 15.0f},
                 {0.0f, -11.0f},
             }}) {
            prefab.beginEntity(gc::Name("spawn_point"), gameplay, {p.x, p.y, 0.05f});
            SpawnPointComponent spawn_point{};
            spawn_point.yaw = std::atan2(-p.x, -p.y);
            prefab.addComponent(spawn_point);
        }

        struct Pickup {
            glm::vec3 position;
            PickupKind kind;
        };
        for (const Pickup& p : std::array<Pickup, 13>{{
                 // embers on the ground
                 {{-10.0f, -6.0f, 1.0f}, PickupKind::EMBER},
                 {{4.0f, -17.0f, 1.0f}, PickupKind::EMBER},
                 {{-3.0f, 15.5f, 1.0f}, PickupKind::EMBER},
                 {{16.0f, 4.0f, 1.0f}, PickupKind::EMBER},
                 {{-19.0f, 16.0f, 1.0f}, PickupKind::EMBER},
                 {{12.0f, -17.0f, 1.0f}, PickupKind::EMBER},
                 {{-13.0f, 21.0f, 1.0f}, PickupKind::EMBER},
                 {{13.0f, 21.0f, 1.0f}, PickupKind::EMBER},
                 // and up high, for those who find the ramps
                 {{0.0f, -3.2f, 2.5f}, PickupKind::EMBER},
                 {{19.0f, -19.0f, 4.2f}, PickupKind::EMBER},
                 {{0.0f, 3.2f, 2.5f}, PickupKind::HEALTH},
                 {{-19.5f, -8.0f, 1.0f}, PickupKind::HEALTH},
                 {{21.0f, 21.0f, 1.0f}, PickupKind::HEALTH},
             }}) {
            prefab.beginEntity(gc::Name(p.kind == PickupKind::EMBER ? "ember_spawner" : "health_spawner"), gameplay, p.position);
            PickupSpawnerComponent spawner{};
            spawner.kind = p.kind;
            spawner.respawn_time = (p.kind == PickupKind::EMBER) ? 8.0f : 15.0f;
            prefab.addComponent(spawner);
        }

        // Two sentinels: one goes round the keep above head height, the other round the walls, higher up, the other way.
        const std::array<std::array<glm::vec3, 4>, 2> routes{{
            {{{-12.0f, -12.0f, 3.3f}, {12.0f, -12.0f, 3.3f}, {12.0f, 12.0f, 3.3f}, {-12.0f, 12.0f, 3.3f}}},
            {{{-20.0f, 16.0f, 8.0f}, {-20.0f, -20.0f, 8.0f}, {20.0f, -20.0f, 8.0f}, {20.0f, 16.0f, 8.0f}}},
        }};
        for (uint32_t route = 0; route < routes.size(); ++route) {
            for (uint32_t order = 0; order < routes[route].size(); ++order) {
                prefab.beginEntity(gc::Name("patrol_point"), gameplay, routes[route][order]);
                PatrolPointComponent point{};
                point.route = route;
                point.order = order;
                prefab.addComponent(point);
            }
        }
    }

    const auto prefab_data = prefab.getData();
    map.addAsset(MAP_PREFAB_NAME, std::vector<uint8_t>(prefab_data.begin(), prefab_data.end()), gcpak::GcpakAssetType::PREFAB);
    std::printf("%u entities, %zu assets\n", prefab.getEntityCount(), map.getAssets().size());
    return map.getAssets();
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::printf("usage: ember_court_mapgen OUTPUT.gcpak\n");
        return EXIT_FAILURE;
    }
    const std::filesystem::path output = argv[1];

    gcpak::GcpakCreator creator{};
    for (const Asset& asset : buildMap()) {
        creator.addAsset(asset);
    }
    if (!creator.saveFile(output)) {
        std::printf("Failed to save %s\n", output.string().c_str());
        return EXIT_FAILURE;
    }
    std::printf("Saved %s\n", output.string().c_str());
    return EXIT_SUCCESS;
}
