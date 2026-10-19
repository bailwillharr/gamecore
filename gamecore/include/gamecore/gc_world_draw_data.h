#pragma once

#include <vector>
#include <span>

#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>

namespace gc {

class RenderMaterial; // forward-dec
class RenderMesh;     // forward-dec
class RenderTexture;  // forward-dec
class RenderShadowMap; // forward-dec
class RenderEnvironment; // forward-dec
class RenderTerrain;  // forward-dec

struct WorldDrawEntry {
    glm::mat4 world_matrix;
    RenderMesh* mesh;
    RenderMaterial* material;
};

struct WorldPointLight {
    glm::vec3 position;
    float range;     // zero: unlimited
    glm::vec3 color; // includes the intensity
};

struct WorldInstancedDrawEntry {
    uint32_t transform_offset; // start index in m_draw_instance_transforms
    uint32_t instance_count;
    RenderMesh* mesh;
    RenderMaterial* material;
};

// One square patch of a terrain, drawn from a tile in a RenderTerrain: one instance of the terrain's instanced draw. The vertex
// shader (content/shader_src/terrain.vert) reads the heights from the tile. See TerrainSystem for how patches are chosen.
// This is the layout of the per-instance vertex attributes.
struct TerrainPatch {
    uint32_t tile;          // the offset of the patch's tile in the RenderTerrain's buffer, in samples
    uint32_t parent_tile;   // the same, for the tile of the level above, which the patch turns into with distance. Itself at the top
    uint32_t origin;        // bits 0-11: the x of the patch's first sample in its tile, 12-23: y, 24-27: the level, 28: draw the
                            // level's colour instead of the material (for debugging)
    uint32_t parent_origin; // the x (low 16 bits) and y (high 16) of the tile's first sample in its parent's tile, in the parent's samples
    float x;                // where the patch's first sample is in the terrain's space, in metres
    float y;
    float spacing;     // between the patch's samples, in metres
    float skirt_depth; // see TerrainNode
    float morph_start; // the distance from the camera at which the patch starts turning into its parent (in world units)
    float morph_end;   // and is its parent. The same as morph_start for a patch that never turns
};
static_assert(sizeof(TerrainPatch) == 40);

struct WorldTerrainDrawEntry {
    RenderTerrain* terrain;
    RenderMaterial* material;
    glm::mat4 world_matrix;
    float height_offset; // see TerrainHeader
    float height_scale;
    uint32_t first_patch; // in getTerrainPatches()
    uint32_t patch_count;
};

// Haze, see FogComponent
struct WorldFog {
    float density{};        // per metre, at base_height. Zero: no fog
    float height_falloff{}; // per metre
    float base_height{};
    glm::vec3 color{};      // luminance (cd/m^2), used if there is no sky to fade to
    bool use_sky{};
};

class WorldDrawData {
public:
    // How many point lights the renderer can draw with. Any more than this are left out.
    static constexpr size_t MAX_POINT_LIGHTS = 32;

private:
    std::vector<WorldDrawEntry> m_draw_entries{};

    std::vector<WorldInstancedDrawEntry> m_instanced_draw_entries{};
    std::vector<glm::mat4> m_instanced_draw_transforms{};

    std::vector<WorldTerrainDrawEntry> m_terrain_draw_entries{};
    std::vector<TerrainPatch> m_terrain_patches{};

    glm::mat4 m_projection_matrix{};
    glm::mat4 m_view_matrix{};

    float m_exposure{1.0f}; // luminance (cd/m^2) is multiplied by this before it is tone mapped

    // A world is lit by one directional light and any number of point lights. Intensities are in lux and candela.
    glm::vec3 m_directional_light_direction{0.0f, 0.0f, 1.0f}; // towards the light
    glm::vec3 m_directional_light_color{0.0f, 0.0f, 0.0f};     // includes the intensity. Black if there is no directional light
    std::vector<WorldPointLight> m_point_lights{};
    glm::vec3 m_ambient_light{0.0f, 0.0f, 0.0f}; // the illuminance (lux) of a surface facing up, from every direction

    // The directional light's shadows. Null if there are none
    RenderShadowMap* m_shadow_map{};
    glm::mat4 m_shadow_matrix{1.0f}; // world space to the shadow map's texture coordinates (x, y) and depth (z)
    float m_shadow_normal_bias{};
    float m_shadow_depth_bias{};

    // The sky, and the light it gives. Null if there isn't one
    RenderEnvironment* m_environment{};
    glm::mat4 m_environment_matrix{1.0f}; // turns a direction in the world to one in the sky's cube maps
    float m_environment_scale{};          // the luminance (cd/m^2) of a texel of 1
    bool m_environment_visible{};         // whether the sky is drawn behind everything
    bool m_environment_lighting{};        // whether the world is lit by it. If so, ambient light isn't drawn

    WorldFog m_fog{};

public:
    void reset()
    {
        m_draw_entries.clear();
        m_instanced_draw_entries.clear();
        m_instanced_draw_transforms.clear();
        m_terrain_draw_entries.clear();
        m_terrain_patches.clear();
        m_directional_light_direction = glm::vec3{0.0f, 0.0f, 1.0f};
        m_directional_light_color = glm::vec3{0.0f, 0.0f, 0.0f};
        m_point_lights.clear();
        m_ambient_light = glm::vec3{0.0f, 0.0f, 0.0f};
        m_shadow_map = nullptr;
        m_environment = nullptr;
        m_fog = WorldFog{};
    }

    void drawMesh(const glm::mat4& world_matrix, RenderMesh* const mesh, RenderMaterial* const material)
    {
        m_draw_entries.emplace_back(world_matrix, mesh, material);
    }
    void drawMeshInstanced(const std::span<const glm::mat4> transforms, RenderMesh* const mesh, RenderMaterial* const material)
    {
        const auto transform_offset = static_cast<uint32_t>(m_instanced_draw_transforms.size());
        const auto instance_count = static_cast<uint32_t>(transforms.size());
        m_instanced_draw_entries.emplace_back(transform_offset, instance_count, mesh, material);
        m_instanced_draw_transforms.insert(m_instanced_draw_transforms.end(), transforms.begin(), transforms.end());
    }

    // The terrain and the material must stay alive until the frame has been submitted. Patches whose tiles haven't been uploaded
    // must not be given.
    void drawTerrain(RenderTerrain* const terrain, RenderMaterial* const material, const glm::mat4& world_matrix, float height_offset,
                     float height_scale, std::span<const TerrainPatch> patches)
    {
        const auto first_patch = static_cast<uint32_t>(m_terrain_patches.size());
        m_terrain_draw_entries.emplace_back(terrain, material, world_matrix, height_offset, height_scale, first_patch,
                                            static_cast<uint32_t>(patches.size()));
        m_terrain_patches.insert(m_terrain_patches.end(), patches.begin(), patches.end());
    }

    void setProjectionMatrix(const glm::mat4& projection_matrix) { m_projection_matrix = projection_matrix; }
    void setViewMatrix(const glm::mat4& view_matrix) { m_view_matrix = view_matrix; }
    void setExposure(float exposure) { m_exposure = exposure; }

    // direction is a unit vector pointing towards the light. Replaces the previous directional light, if any
    void setDirectionalLight(const glm::vec3& direction, const glm::vec3& color)
    {
        m_directional_light_direction = direction;
        m_directional_light_color = color;
    }
    void addPointLight(const glm::vec3& position, float range, const glm::vec3& color) { m_point_lights.emplace_back(position, range, color); }
    void setAmbientLight(const glm::vec3& ambient_light) { m_ambient_light = ambient_light; }
    // The shadow map must stay alive until the frame has been submitted. Null removes the shadows.
    // See ShadowMapComponent for what the biases are
    void setShadowMap(RenderShadowMap* shadow_map, const glm::mat4& world_to_shadow_map = glm::mat4{1.0f}, float normal_bias = 0.0f, float depth_bias = 0.0f)
    {
        m_shadow_map = shadow_map;
        m_shadow_matrix = world_to_shadow_map;
        m_shadow_normal_bias = normal_bias;
        m_shadow_depth_bias = depth_bias;
    }

    // The environment must stay alive until the frame has been submitted. Null removes the sky.
    // world_to_sky only turns: the sky is infinitely far away. See SkyboxComponent
    void setEnvironment(RenderEnvironment* environment, const glm::mat4& world_to_sky = glm::mat4{1.0f}, float scale = 0.0f, bool visible = true,
                        bool lighting = true)
    {
        m_environment = environment;
        m_environment_matrix = world_to_sky;
        m_environment_scale = scale;
        m_environment_visible = visible;
        m_environment_lighting = lighting;
    }
    void setEnvironmentVisible(bool visible) { m_environment_visible = visible; }
    void setFog(const WorldFog& fog) { m_fog = fog; }
    void setEnvironmentLighting(bool lighting) { m_environment_lighting = lighting; }

    const auto& getDrawEntries() const { return m_draw_entries; }
    const auto& getInstancedDrawEntries() const { return m_instanced_draw_entries; }
    const auto& getInstancedDrawTransforms() const { return m_instanced_draw_transforms; }
    const auto& getTerrainDrawEntries() const { return m_terrain_draw_entries; }
    const auto& getTerrainPatches() const { return m_terrain_patches; }

    const auto& getProjectionMatrix() const { return m_projection_matrix; }
    const auto& getViewMatrix() const { return m_view_matrix; }
    float getExposure() const { return m_exposure; }
    const auto& getDirectionalLightDirection() const { return m_directional_light_direction; }
    const auto& getDirectionalLightColor() const { return m_directional_light_color; }
    const auto& getPointLights() const { return m_point_lights; }
    const auto& getAmbientLight() const { return m_ambient_light; }
    RenderShadowMap* getShadowMap() const { return m_shadow_map; }
    const auto& getShadowMatrix() const { return m_shadow_matrix; }
    float getShadowNormalBias() const { return m_shadow_normal_bias; }
    float getShadowDepthBias() const { return m_shadow_depth_bias; }
    RenderEnvironment* getEnvironment() const { return m_environment; }
    const auto& getEnvironmentMatrix() const { return m_environment_matrix; }
    float getEnvironmentScale() const { return m_environment_scale; }
    bool getEnvironmentVisible() const { return m_environment_visible; }
    bool getEnvironmentLighting() const { return m_environment_lighting; }
    const WorldFog& getFog() const { return m_fog; }
};

} // namespace gc
