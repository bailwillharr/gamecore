#pragma once

// Small geometric tests that the collision code shares: the CollisionSystem's meshes, and terrains (gc_terrain.cpp).

#include <algorithm>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "gamecore/gc_collision_bvh.h"

namespace gc {

inline AABB boxUnion(const AABB& a, const AABB& b) { return AABB{glm::min(a.min, b.min), glm::max(a.max, b.max)}; }

inline void boxExpand(AABB& box, const glm::vec3& point)
{
    box.min = glm::min(box.min, point);
    box.max = glm::max(box.max, point);
}

// the surface area: boxes with less of it are hit by fewer rays, which is what makes a good tree
inline float boxArea(const AABB& box)
{
    const glm::vec3 size = box.max - box.min;
    return 2.0f * (size.x * size.y + size.y * size.z + size.z * size.x);
}

inline bool boxesOverlap(const AABB& a, const AABB& b)
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y && a.min.z <= b.max.z && a.max.z >= b.min.z;
}

// the box around a box that has been transformed
inline AABB transformBox(const AABB& box, const glm::mat4& matrix)
{
    AABB result{};
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point{(corner & 1) ? box.max.x : box.min.x, (corner & 2) ? box.max.y : box.min.y, (corner & 4) ? box.max.z : box.min.z};
        boxExpand(result, glm::vec3(matrix * glm::vec4(point, 1.0f)));
    }
    return result;
}

// Whether the ray gets into the box before it has gone max_t. entry_t is how far along the ray it does (zero if it starts inside).
// inverse_direction is 1 / direction, which is infinite for the axes the ray doesn't move along.
inline bool rayHitsBox(const glm::vec3& origin, const glm::vec3& inverse_direction, const AABB& box, float max_t, float& entry_t)
{
    // Thank you https://tavianator.com/cgit/dimension.git/tree/libdimension/bvh/bvh.c
    float t_min = 0.0f;
    float t_max = max_t;
    for (int axis = 0; axis < 3; ++axis) {
        const float t1 = (box.min[axis] - origin[axis]) * inverse_direction[axis];
        const float t2 = (box.max[axis] - origin[axis]) * inverse_direction[axis];
        // written so that a NaN (zero times infinity, when the ray lies in one of the box's faces) leaves the range alone
        t_min = std::max(t_min, std::min(t1, t2));
        t_max = std::min(t_max, std::max(t1, t2));
    }
    entry_t = t_min;
    return t_max >= t_min;
}

// Moller-Trumbore. Hits either side of the triangle. t is in units of the direction's length.
inline bool rayHitsTriangle(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, float& t)
{
    const glm::vec3 edge1 = b - a;
    const glm::vec3 edge2 = c - a;
    const glm::vec3 p = glm::cross(direction, edge2);
    const float determinant = glm::dot(edge1, p);
    if (determinant == 0.0f) {
        return false; // parallel to the triangle, or the triangle has no area
    }
    const float inverse_determinant = 1.0f / determinant;
    const glm::vec3 to_origin = origin - a;
    const float u = glm::dot(to_origin, p) * inverse_determinant;
    if (!(u >= 0.0f && u <= 1.0f)) {
        return false;
    }
    const glm::vec3 q = glm::cross(to_origin, edge1);
    const float v = glm::dot(direction, q) * inverse_determinant;
    if (!(v >= 0.0f && u + v <= 1.0f)) {
        return false;
    }
    t = glm::dot(edge2, q) * inverse_determinant;
    return t >= 0.0f;
}

// From "Real-Time Collision Detection" by Christer Ericson
inline glm::vec3 closestPointOnTriangle(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
{
    const glm::vec3 ab = b - a;
    const glm::vec3 ac = c - a;
    const glm::vec3 ap = p - a;
    const float d1 = glm::dot(ab, ap);
    const float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return a;
    }
    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp);
    const float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return b;
    }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        return a + ab * (d1 / (d1 - d3));
    }
    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp);
    const float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return c;
    }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        return a + ac * (d2 / (d2 - d6));
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const float denominator = va + vb + vc;
    if (denominator == 0.0f) {
        return a; // the triangle has no area
    }
    return a + ab * (vb / denominator) + ac * (vc / denominator);
}

} // namespace gc
