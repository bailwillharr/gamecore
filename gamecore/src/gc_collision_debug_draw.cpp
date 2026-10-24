// CollisionSystem::drawDebug(), the debug UI's collision visualiser. It is kept out of gc_collision_system.cpp as it is only for
// looking at the system, and nothing else in the system uses ImGui.

#include "gamecore/gc_collision_system.h"

#include <cmath>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>
#include <glm/vec4.hpp>

#include <imgui.h>

#include <tracy/Tracy.hpp>

namespace gc {

namespace {

// Lines nearer to the camera than this (in world units) are cut off, as they can't be projected
constexpr float NEAR_W = 0.01f;

// The boxes of a mesh's tree stop being drawn when they are smaller than this on the screen (as are the boxes inside them, which
// are smaller still). Below this, the deep levels of a big mesh's tree are only a smudge of thousands of lines.
constexpr float MIN_BOX_PIXELS = 6.0f;

constexpr ImU32 COLLIDER_COLOUR = IM_COL32(80, 255, 120, 255);
constexpr ImU32 TERRAIN_COLOUR = IM_COL32(255, 170, 60, 255);
constexpr ImU32 TRIANGLE_COLOUR = IM_COL32(90, 200, 255, 150);

// Each level of a tree has its own colour, so that the levels can be told apart when they are all drawn
ImU32 getLevelColour(int level, float alpha)
{
    const float hue = std::fmod(0.55f + 0.17f * static_cast<float>(level), 1.0f);
    return ImColor::HSV(hue, 0.65f, 1.0f, alpha);
}

bool isLevelShown(int level, int shown_level) { return shown_level < 0 || level == shown_level; }

// The corners of a box, in the order that transformBox() uses: bit 0 of the index is x, bit 1 is y and bit 2 is z
std::array<glm::vec3, 8> getBoxCorners(const AABB& box, const glm::mat4& matrix)
{
    std::array<glm::vec3, 8> corners{};
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point{(corner & 1) ? box.max.x : box.min.x, (corner & 2) ? box.max.y : box.min.y, (corner & 4) ? box.max.z : box.min.z};
        corners[corner] = glm::vec3(matrix * glm::vec4(point, 1.0f));
    }
    return corners;
}

// A box seen by the camera
struct ProjectedBox {
    std::array<glm::vec4, 8> corners; // in clip space
    bool visible;                     // whether any of it could be on the screen, and it is near enough to be drawn
    float screen_size;                // the larger of its width and height on the screen, in pixels. Infinite if it reaches behind the camera
};

// Draws lines given in clip space onto ImGui's background draw list, cutting off the parts that are outside the camera's view
class LineDrawer {
    ImDrawList* m_draw_list;
    ImVec2 m_screen_pos;
    ImVec2 m_screen_size;
    glm::mat4 m_view_projection;
    glm::vec3 m_camera_position;
    float m_max_distance;
    int m_max_lines;

public:
    CollisionDebugDrawStats stats{};

public:
    LineDrawer(const glm::mat4& view, const glm::mat4& projection, float max_distance, int max_lines)
        : m_draw_list(ImGui::GetBackgroundDrawList()),
          m_screen_pos(ImGui::GetMainViewport()->Pos),
          m_screen_size(ImGui::GetMainViewport()->Size),
          m_view_projection(projection * view),
          m_camera_position(glm::inverse(view)[3]),
          m_max_distance(max_distance),
          m_max_lines(max_lines)
    {
    }

    const glm::mat4& getViewProjection() const { return m_view_projection; }

    const glm::vec3& getCameraPosition() const { return m_camera_position; }

    // Lines are dropped once there have been max_lines of them
    bool isFull() const { return stats.hit_max_lines; }

    // corners are in world space
    ProjectedBox projectBox(const std::array<glm::vec3, 8>& corners) const
    {
        ProjectedBox result{};

        AABB box{};
        for (const glm::vec3& corner : corners) {
            box.min = glm::min(box.min, corner);
            box.max = glm::max(box.max, corner);
        }
        if (getDistance(box) > m_max_distance) {
            return result;
        }

        // off the screen if all of the corners are on the wrong side of the same edge of the view
        std::array<bool, 5> all_outside{true, true, true, true, true};
        bool behind = false;
        ImVec2 screen_min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        ImVec2 screen_max{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
        for (size_t corner = 0; corner < corners.size(); ++corner) {
            result.corners[corner] = m_view_projection * glm::vec4(corners[corner], 1.0f);
            const std::array<float, 5> distances = getPlaneDistances(result.corners[corner]);
            for (size_t i = 0; i < distances.size(); ++i) {
                all_outside[i] = all_outside[i] && distances[i] < 0.0f;
            }
            if (distances[0] < 0.0f) {
                behind = true;
            }
            else {
                const ImVec2 point = toScreen(result.corners[corner]);
                screen_min = ImVec2(std::min(screen_min.x, point.x), std::min(screen_min.y, point.y));
                screen_max = ImVec2(std::max(screen_max.x, point.x), std::max(screen_max.y, point.y));
            }
        }
        result.visible = std::none_of(all_outside.begin(), all_outside.end(), [](bool b) { return b; });
        result.screen_size = behind ? std::numeric_limits<float>::infinity() : std::max(screen_max.x - screen_min.x, screen_max.y - screen_min.y);
        return result;
    }

    // from the camera to the nearest point of a box in world space (zero if the camera is inside it)
    float getDistance(const AABB& box) const
    {
        return glm::length(glm::max(glm::max(box.min - m_camera_position, m_camera_position - box.max), glm::vec3{0.0f}));
    }

    void drawBox(const ProjectedBox& box, ImU32 colour)
    {
        // an edge joins each pair of corners that differ in one axis
        for (int corner = 0; corner < 8; ++corner) {
            for (int axis_bit = 1; axis_bit < 8; axis_bit <<= 1) {
                if (!(corner & axis_bit)) {
                    drawLine(box.corners[corner], box.corners[corner | axis_bit], colour);
                }
            }
        }
    }

    // a and b are in clip space
    void drawLine(const glm::vec4& a, const glm::vec4& b, ImU32 colour)
    {
        if (stats.lines >= m_max_lines) {
            stats.hit_max_lines = true;
            return;
        }

        // Liang-Barsky: cut the line down to the part that is inside every plane
        const std::array<float, 5> distances_a = getPlaneDistances(a);
        const std::array<float, 5> distances_b = getPlaneDistances(b);
        float t0 = 0.0f;
        float t1 = 1.0f;
        for (size_t i = 0; i < distances_a.size(); ++i) {
            const float da = distances_a[i];
            const float db = distances_b[i];
            if (da < 0.0f && db < 0.0f) {
                return;
            }
            if (da < 0.0f) {
                t0 = std::max(t0, da / (da - db));
            }
            else if (db < 0.0f) {
                t1 = std::min(t1, da / (da - db));
            }
        }
        if (t0 > t1) {
            return;
        }

        m_draw_list->AddLine(toScreen(a + (b - a) * t0), toScreen(a + (b - a) * t1), colour);
        ++stats.lines;
    }

private:
    // How far inside each plane of the view a point in clip space is: the near plane, then the left, right, bottom and top edges.
    // (The camera's projection has no far plane.)
    static std::array<float, 5> getPlaneDistances(const glm::vec4& p) { return {p.w - NEAR_W, p.w + p.x, p.w - p.x, p.w + p.y, p.w - p.y}; }

    ImVec2 toScreen(const glm::vec4& clip) const
    {
        const float x = clip.x / clip.w;
        const float y = clip.y / clip.w; // the projection already flips y, so -1 is the top of the screen
        return ImVec2(m_screen_pos.x + (x * 0.5f + 0.5f) * m_screen_size.x, m_screen_pos.y + (y * 0.5f + 0.5f) * m_screen_size.y);
    }
};

} // namespace

CollisionDebugDrawStats CollisionSystem::drawDebug(const CollisionDebugDrawOptions& options, const glm::mat4& view, const glm::mat4& projection) const
{
    ZoneScoped;

    const bool draw_broad_phase = options.collider_boxes || options.broad_phase_tree;
    const bool draw_meshes = options.mesh_triangles || options.mesh_trees;
    if (!ImGui::GetCurrentContext() || (!draw_broad_phase && !draw_meshes)) {
        return {};
    }

    LineDrawer drawer(view, projection, options.max_distance, options.max_lines);

    // The broad phase tree. A node that is off the screen or too far away is skipped along with everything under it, as its
    // box holds theirs.
    if (draw_broad_phase && m_tree_root != -1) {
        std::vector<std::pair<int32_t, int>> stack{{m_tree_root, 0}};
        while (!stack.empty() && !drawer.isFull()) {
            const auto [index, level] = stack.back();
            stack.pop_back();
            const TreeNode& node = m_tree[index];
            const ProjectedBox box = drawer.projectBox(getBoxCorners(node.box, glm::mat4{1.0f}));
            if (!box.visible) {
                continue;
            }
            if (node.child1 == -1) {
                if (options.collider_boxes) {
                    drawer.drawBox(box, node.is_terrain ? TERRAIN_COLOUR : COLLIDER_COLOUR);
                }
                continue;
            }
            if (options.broad_phase_tree && isLevelShown(level, options.broad_phase_level)) {
                drawer.drawBox(box, getLevelColour(level, 1.0f));
            }
            stack.emplace_back(node.child1, level + 1);
            stack.emplace_back(node.child2, level + 1);
        }
    }

    // The narrow phase: each collider's tree of triangles, in the collider's place. The nearest colliders go first, so that they
    // are the ones that are drawn if there are too many lines. (Terrains have no triangles to show: they are heightmaps.)
    if (draw_meshes) {
        std::vector<std::pair<float, const Collider*>> nearest{};
        for (const auto& [entity, collider] : m_colliders) {
            if (collider.tree_node == -1) {
                continue; // no triangles
            }
            const float distance = drawer.getDistance(m_tree[collider.tree_node].box);
            if (distance <= options.max_distance) {
                nearest.emplace_back(distance, &collider);
            }
        }
        std::sort(nearest.begin(), nearest.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

        struct StackEntry {
            uint32_t node;
            int level;
            bool draw_boxes; // false below a box that was too small to see
        };

        for (const auto& [distance, collider] : nearest) {
            if (drawer.isFull()) {
                break;
            }
            const CollisionBvhView& shape = collider->shape->bvh;
            const glm::mat4 clip_matrix = drawer.getViewProjection() * collider->world_matrix;

            std::array<StackEntry, 64> stack{};
            size_t stack_size = 0;
            stack[stack_size++] = {0, 0, options.mesh_trees};
            while (stack_size > 0 && !drawer.isFull()) {
                const StackEntry entry = stack[--stack_size];
                const CollisionBvhNode& node = shape.nodes[entry.node];
                const ProjectedBox box = drawer.projectBox(getBoxCorners(node.box, collider->world_matrix));
                if (!box.visible) {
                    continue;
                }
                const bool draw_boxes = entry.draw_boxes && box.screen_size >= MIN_BOX_PIXELS;
                if (draw_boxes && isLevelShown(entry.level, options.mesh_tree_level)) {
                    drawer.drawBox(box, getLevelColour(entry.level, 0.8f));
                }
                if (node.count == 0) {
                    // (once the boxes are too small, only the triangles are left to go down for)
                    if ((draw_boxes || options.mesh_triangles) && stack_size + 2 <= stack.size()) {
                        stack[stack_size++] = {node.first, entry.level + 1, draw_boxes};
                        stack[stack_size++] = {node.first + 1, entry.level + 1, draw_boxes};
                    }
                    continue;
                }
                if (options.mesh_triangles) {
                    for (uint32_t i = 0; i < node.count; ++i) {
                        const uint16_t* const indices = &shape.triangles[static_cast<size_t>(node.first + i) * 3];
                        const glm::vec4 a = clip_matrix * glm::vec4(shape.positions[indices[0]], 1.0f);
                        const glm::vec4 b = clip_matrix * glm::vec4(shape.positions[indices[1]], 1.0f);
                        const glm::vec4 c = clip_matrix * glm::vec4(shape.positions[indices[2]], 1.0f);
                        drawer.drawLine(a, b, TRIANGLE_COLOUR);
                        drawer.drawLine(b, c, TRIANGLE_COLOUR);
                        drawer.drawLine(c, a, TRIANGLE_COLOUR);
                    }
                }
            }
        }
    }

    return drawer.stats;
}

} // namespace gc
