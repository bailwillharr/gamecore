#include "gamecore/gc_world_ui.h"

#include <cstdint>

#include <format>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include <imgui.h>

#include "gamecore/gc_byte_writer.h"
#include "gamecore/gc_camera_component.h"
#include "gamecore/gc_collider_component.h"
#include "gamecore/gc_light_component.h"
#include "gamecore/gc_renderable_component.h"
#include "gamecore/gc_replication.h"
#include "gamecore/gc_shadow_map_component.h"
#include "gamecore/gc_transform_component.h"
#include "gamecore/gc_transform_system.h"
#include "gamecore/gc_world.h"

namespace gc {

namespace {

std::unordered_map<Name, ComponentViewer>& getViewers()
{
    static std::unordered_map<Name, ComponentViewer> s_viewers{};
    return s_viewers;
}

// Names are only hashes. Their strings are known if the name has been seen as a string (see Name::getString())
void textName(const char* label, Name name)
{
    if (name.empty()) {
        ImGui::Text("%s: (none)", label);
    }
    else {
        ImGui::Text("%s: %s", label, name.getString().c_str());
    }
}

std::string getEntityLabel(const TransformComponent& transform, Entity entity) { return std::format("{}  #{}", transform.name.getString(), entity); }

//
// Viewers for the engine's components
//

void viewTransform(World& world, Entity entity)
{
    TransformComponent& t = *world.getComponent<TransformComponent>(entity);

    glm::vec3 position = t.getPosition();
    if (ImGui::DragFloat3("Position", &position.x, 0.05f)) {
        t.setPosition(position);
    }
    glm::vec3 scale = t.getScale();
    if (ImGui::DragFloat3("Scale", &scale.x, 0.01f)) {
        t.setScale(scale);
    }
    const glm::quat rotation = t.getRotation();
    const glm::vec3 euler = glm::degrees(glm::eulerAngles(rotation));
    ImGui::Text("Rotation: %.1f %.1f %.1f degrees (XYZ)", euler.x, euler.y, euler.z);
    ImGui::Text("Quaternion: w %.3f  x %.3f  y %.3f  z %.3f", rotation.w, rotation.x, rotation.y, rotation.z);
    if (ImGui::Button("Reset rotation")) {
        t.setRotation(glm::quat{1.0f, 0.0f, 0.0f, 0.0f});
    }

    ImGui::Separator();
    const glm::vec3 world_position = t.getWorldPosition();
    ImGui::Text("World position: %.2f %.2f %.2f", world_position.x, world_position.y, world_position.z);
    if (t.getParent() == ENTITY_NONE) {
        ImGui::TextUnformatted("Parent: (none)");
    }
    else if (const TransformComponent* const parent = world.getComponent<TransformComponent>(t.getParent()); parent) {
        ImGui::Text("Parent: %s", getEntityLabel(*parent, t.getParent()).c_str());
    }
}

void viewRenderable(World& world, Entity entity)
{
    RenderableComponent& r = *world.getComponent<RenderableComponent>(entity);
    ImGui::Checkbox("Visible", &r.m_visible);
    textName("Mesh", r.m_mesh);
    textName("Material", r.m_material);
}

void viewCamera(World& world, Entity entity)
{
    CameraComponent& c = *world.getComponent<CameraComponent>(entity);
    bool active = c.getActive();
    if (ImGui::Checkbox("Active", &active)) {
        c.setActive(active);
    }
    float fov_degrees = glm::degrees(c.getFOV());
    if (ImGui::SliderFloat("FOV", &fov_degrees, 10.0f, 150.0f, "%.1f degrees")) {
        c.setFOV(glm::radians(fov_degrees));
    }
    float near_plane = c.getNearPlane();
    if (ImGui::DragFloat("Near plane", &near_plane, 0.005f, 0.001f, 100.0f, "%.3f m")) {
        c.setNearPlane(near_plane);
    }
    float exposure = c.getExposure();
    if (ImGui::DragFloat("Exposure", &exposure, 0.05f, -10.0f, 25.0f, "EV %.1f")) {
        c.setExposure(exposure);
    }
}

void viewLight(World& world, Entity entity)
{
    LightComponent& l = *world.getComponent<LightComponent>(entity);

    int type = static_cast<int>(l.m_type);
    if (ImGui::Combo("Type", &type, "Point\0Directional\0Ambient\0")) {
        l.m_type = static_cast<LightType>(type);
    }
    ImGui::ColorEdit3("Color", &l.m_color.r, ImGuiColorEditFlags_Float);
    const char* const unit = (l.m_type == LightType::POINT) ? "%.1f cd" : "%.1f lux";
    ImGui::DragFloat("Intensity", &l.m_intensity, 1.0f, 0.0f, 1.0e7f, unit, ImGuiSliderFlags_Logarithmic);
    if (l.m_type == LightType::POINT) {
        ImGui::DragFloat("Range", &l.m_range, 0.1f, 0.0f, 1.0e4f, (l.m_range > 0.0f) ? "%.1f m" : "unlimited");
    }
}

void viewShadowMap(World& world, Entity entity)
{
    ShadowMapComponent& s = *world.getComponent<ShadowMapComponent>(entity);
    textName("Shadow map", s.m_shadow_map);
    ImGui::DragFloat("Normal bias", &s.m_normal_bias, 0.001f, 0.0f, 10.0f, "%.4f m");
    ImGui::DragFloat("Depth bias", &s.m_depth_bias, 0.00001f, 0.0f, 1.0f, "%.6f");
}

void viewCollider(World& world, Entity entity)
{
    const ColliderComponent& c = *world.getComponent<ColliderComponent>(entity);
    textName("Mesh", c.m_mesh);
}

void viewReplicated(World& world, Entity entity)
{
    const ReplicatedComponent& r = *world.getComponent<ReplicatedComponent>(entity);
    ImGui::Text("Net ID: %u", r.net_id);
    textName("Archetype", r.archetype);
    if (r.owner == NET_PEER_NONE) {
        ImGui::TextUnformatted("Owner: (none)");
    }
    else {
        ImGui::Text("Owner: peer %u", r.owner);
    }
}

void registerEngineViewers()
{
    auto& viewers = getViewers();
    // try_emplace, so that a viewer the game registered first isn't replaced
    viewers.try_emplace(TransformComponent::NAME, viewTransform);
    viewers.try_emplace(RenderableComponent::NAME, viewRenderable);
    viewers.try_emplace(CameraComponent::NAME, viewCamera);
    viewers.try_emplace(LightComponent::NAME, viewLight);
    viewers.try_emplace(ShadowMapComponent::NAME, viewShadowMap);
    viewers.try_emplace(ColliderComponent::NAME, viewCollider);
    viewers.try_emplace(ReplicatedComponent::NAME, viewReplicated);
}

// For components that have no viewer
void viewSerialisedBytes(World& world, Entity entity, Name component_name)
{
    const auto size = world.getComponentSerialisedSize(component_name);
    if (!size) {
        ImGui::TextDisabled("No viewer for this component");
        return;
    }
    std::vector<uint8_t> bytes(*size);
    ByteWriter writer(bytes);
    if (!world.serialiseComponent(entity, component_name, writer)) {
        return;
    }
    ImGui::TextDisabled("No viewer for this component. Serialised (%u bytes):", static_cast<unsigned>(bytes.size()));
    constexpr size_t MAX_BYTES_SHOWN = 256;
    std::string line{};
    for (size_t i = 0; i < bytes.size() && i < MAX_BYTES_SHOWN; ++i) {
        line += std::format("{:02x} ", bytes[i]);
        if (i % 16 == 15 || i + 1 == bytes.size() || i + 1 == MAX_BYTES_SHOWN) {
            ImGui::TextUnformatted(line.c_str());
            line.clear();
        }
    }
    if (bytes.size() > MAX_BYTES_SHOWN) {
        ImGui::TextDisabled("...");
    }
}

// One entity in the tree, and its children if it is open
void renderEntityNode(World& world, const TransformSystem& transform_system, Entity entity, Entity& selected)
{
    const TransformComponent* const transform = world.getComponent<TransformComponent>(entity);
    if (!transform) {
        return;
    }
    const std::span<const Entity> children = transform_system.getChildren(entity);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (entity == selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(entity)), flags, "%s", getEntityLabel(*transform, entity).c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selected = entity;
    }
    if (open && !children.empty()) {
        for (const Entity child : children) {
            renderEntityNode(world, transform_system, child, selected);
        }
        ImGui::TreePop();
    }
}

void renderInspector(World& world, Entity entity)
{
    const TransformComponent* const transform = world.getComponent<TransformComponent>(entity);
    if (!transform) {
        return;
    }
    ImGui::Text("%s", transform->name.getString().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("entity %u", entity);

    const auto& viewers = getViewers();
    for (const Name component_name : world.getComponentList(entity)) {
        ImGui::PushID(static_cast<int>(component_name.getHash()));
        // the transform is what is wanted most often
        const ImGuiTreeNodeFlags flags = (component_name == TransformComponent::NAME) ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
        if (ImGui::CollapsingHeader(component_name.getString().c_str(), flags)) {
            if (const auto it = viewers.find(component_name); it != viewers.end()) {
                it->second(world, entity);
            }
            else {
                viewSerialisedBytes(world, entity, component_name);
            }
        }
        ImGui::PopID();
    }
}

} // namespace

void registerComponentViewer(Name component_name, ComponentViewer viewer) { getViewers().insert_or_assign(component_name, viewer); }

void renderWorldUI(World& world, bool* open)
{
    if (!open) {
        return;
    }

    static Entity s_selected{ENTITY_NONE};
    static ImGuiTextFilter s_filter{};
    static bool s_viewers_registered{false};
    if (!s_viewers_registered) {
        registerEngineViewers();
        s_viewers_registered = true;
    }

    ImGui::SetNextWindowPos(ImVec2(20.0f, 40.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(680.0f, 460.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("World", open)) {
        // the entity that was selected may have been deleted since
        if (s_selected != ENTITY_NONE && !world.isEntityAlive(s_selected)) {
            s_selected = ENTITY_NONE;
        }

        std::vector<Entity> roots{};
        uint32_t entity_count = 0;
        world.forEach<TransformComponent>([&](Entity entity, const TransformComponent& transform) {
            ++entity_count;
            if (transform.getParent() == ENTITY_NONE) {
                roots.push_back(entity);
            }
        });

        s_filter.Draw("Filter", 200.0f);
        ImGui::SameLine();
        ImGui::TextDisabled("%u entities", entity_count);

        // the hierarchy on the left, the selected entity's components on the right
        ImGui::BeginChild("hierarchy", ImVec2(280.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        if (s_filter.IsActive()) {
            // a flat list of the entities whose names match
            world.forEach<TransformComponent>([&](Entity entity, const TransformComponent& transform) {
                const std::string label = getEntityLabel(transform, entity);
                if (s_filter.PassFilter(label.c_str())) {
                    ImGui::PushID(static_cast<int>(entity));
                    if (ImGui::Selectable(label.c_str(), entity == s_selected)) {
                        s_selected = entity;
                    }
                    ImGui::PopID();
                }
            });
        }
        else {
            const TransformSystem& transform_system = world.getSystem<TransformSystem>();
            for (const Entity root : roots) {
                renderEntityNode(world, transform_system, root, s_selected);
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("inspector", ImVec2(0.0f, 0.0f));
        if (s_selected == ENTITY_NONE) {
            ImGui::TextDisabled("Select an entity to see its components");
        }
        else {
            renderInspector(world, s_selected);
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace gc
