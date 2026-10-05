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

class WorldDrawData {
public:
    // How many point lights the renderer can draw with. Any more than this are left out.
    static constexpr size_t MAX_POINT_LIGHTS = 32;

private:
    std::vector<WorldDrawEntry> m_draw_entries{};

    std::vector<WorldInstancedDrawEntry> m_instanced_draw_entries{};
    std::vector<glm::mat4> m_instanced_draw_transforms{};

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

public:
    void reset()
    {
        m_draw_entries.clear();
        m_instanced_draw_entries.clear();
        m_instanced_draw_transforms.clear();
        m_directional_light_direction = glm::vec3{0.0f, 0.0f, 1.0f};
        m_directional_light_color = glm::vec3{0.0f, 0.0f, 0.0f};
        m_point_lights.clear();
        m_ambient_light = glm::vec3{0.0f, 0.0f, 0.0f};
        m_shadow_map = nullptr;
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

    const auto& getDrawEntries() const { return m_draw_entries; }
    const auto& getInstancedDrawEntries() const { return m_instanced_draw_entries; }
    const auto& getInstancedDrawTransforms() const { return m_instanced_draw_transforms; }

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
};

} // namespace gc
