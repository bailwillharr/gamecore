#include "flythrough.h"

#include <cmath>

#include <algorithm>
#include <numeric>

#include <glm/geometric.hpp>

#include <tracy/Tracy.hpp>

#include <gclog/gclog.h>

#include <gamecore/gc_app.h>
#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_terrain.h>
#include <gamecore/gc_terrain_system.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_units.h>
#include <gamecore/gc_world.h>

#include "explorer.h"

FlythroughSystem::FlythroughSystem(gc::World& world, gc::App& app, const gc::ResourceTerrain& terrain, gc::TerrainSystem& terrain_system, gc::Entity camera,
                                   const Sites& sites, float seconds)
    : gc::System(world), m_app(app), m_terrain(terrain), m_terrain_system(terrain_system), m_camera(camera), m_duration(seconds)
{
    // Along the fissure, which runs roughly north to south through the deepest point. (Kept to the scan, so that the ground is
    // detailed all the way)
    const glm::vec2 data_min = terrain.getDataMin() + glm::vec2{300.0f};
    const glm::vec2 data_max = terrain.getDataMax() - glm::vec2{300.0f};
    const auto clamp_to_data = [&](glm::vec2 p) { return glm::clamp(p, data_min, data_max); };
    const glm::vec2 deepest{sites.deepest};
    const glm::vec2 north = clamp_to_data(deepest + glm::vec2{0.0f, 3500.0f});
    const glm::vec2 south = clamp_to_data(deepest - glm::vec2{0.0f, 3500.0f});
    const glm::vec2 centre = 0.5f * (terrain.getDataMin() + terrain.getDataMax());

    // x, y, height above the ground
    const std::vector<glm::vec3> places{
        {glm::vec2{sites.spawn}, EXPLORER_EYE_HEIGHT}, // where a walker starts
        {glm::mix(glm::vec2{sites.spawn}, deepest, 0.5f), 40.0f},
        {deepest, 15.0f},  // down into the fissure
        {north, 40.0f},    // low and fast along it
        {centre, 2500.0f}, // up, to see all of it
        {south, 25.0f},    // and down again
        {deepest, 60.0f},
    };
    // Time for each leg in proportion to its length, so the speed is about the same all the way
    std::vector<float> lengths{0.0f};
    for (size_t i = 1; i < places.size(); ++i) {
        lengths.push_back(glm::distance(glm::vec2(places[i]), glm::vec2(places[i - 1])) + std::abs(places[i].z - places[i - 1].z));
    }
    const float total = std::accumulate(lengths.begin(), lengths.end(), 0.0f);
    for (size_t i = 0; i < places.size(); ++i) {
        m_route.push_back(Waypoint{places[i], m_duration * lengths[i] / total});
    }
    GC_INFO("Flythrough: {:.1f} km in {:.0f} s ({:.0f} m/s)", total / 1000.0f, m_duration, total / m_duration);
}

glm::vec3 FlythroughSystem::getPlace(double time) const
{
    float t = static_cast<float>(time);
    glm::vec3 place = m_route.back().place;
    for (size_t i = 1; i < m_route.size(); ++i) {
        if (t <= m_route[i].seconds) {
            // eased at both ends of each leg
            float f = (m_route[i].seconds > 0.0f) ? t / m_route[i].seconds : 1.0f;
            f = f * f * (3.0f - 2.0f * f);
            place = glm::mix(m_route[i - 1].place, m_route[i].place, f);
            break;
        }
        t -= m_route[i].seconds;
    }
    const float ground = m_terrain.getHeight(place.x, place.y).value_or(m_terrain.header.min_height);
    return glm::vec3{place.x, place.y, ground + place.z};
}

void FlythroughSystem::onUpdate(gc::FrameState& frame_state)
{
    ZoneScoped;

    gc::TransformComponent* const transform = m_world.getComponent<gc::TransformComponent>(m_camera);
    if (!transform) {
        return;
    }
    // (the first frame's time is how long loading took)
    if (frame_state.frame_count > 0) {
        m_time += frame_state.delta_time;
        const float frame_ms = static_cast<float>(frame_state.delta_time * 1000.0);
        m_frame_times.push_back(frame_ms);
        m_second_frame_times.push_back(frame_ms);
    }

    const glm::vec3 place = getPlace(m_time);
    const glm::vec3 ahead = getPlace(m_time + 1.5);
    glm::vec3 direction = ahead - place;
    if (glm::length(glm::vec2(direction)) < 1.0f) {
        direction = glm::vec3{0.0f, 1.0f, -0.1f}; // (standing still: look north)
    }
    direction = glm::normalize(direction);
    const float yaw = std::atan2(direction.x, direction.y);
    const float pitch = std::clamp(std::asin(direction.z), -0.6f, 0.3f) - 0.08f;
    transform->setPosition(place);
    transform->setRotation(ExplorerSystem::makeLookRotation(yaw, pitch));

    const gc::TerrainSystem::Stats& stats = m_terrain_system.getStats();
    m_second_uploads += stats.uploads_started;
    m_second_most_missing = std::max(m_second_most_missing, stats.missing);
    m_most_missing = std::max(m_most_missing, stats.missing);
    m_most_patches = std::max(m_most_patches, stats.patches);
    m_most_triangles = std::max(m_most_triangles, stats.triangles);
    m_most_slots_used = std::max(m_most_slots_used, stats.resident + stats.loading);

    const double second = std::floor(m_time);
    const double last_second = std::floor(m_time - frame_state.delta_time);
    if (frame_state.frame_count > 0 && second != last_second) {
        logSecond();
    }
    if (m_time >= m_duration) {
        logSummary();
        m_app.requestQuit();
    }
}

void FlythroughSystem::logSecond()
{
    if (m_second_frame_times.empty()) {
        return;
    }
    const float average = std::accumulate(m_second_frame_times.begin(), m_second_frame_times.end(), 0.0f) / static_cast<float>(m_second_frame_times.size());
    const float worst = *std::max_element(m_second_frame_times.begin(), m_second_frame_times.end());
    const gc::TerrainSystem::Stats& stats = m_terrain_system.getStats();
    const glm::vec3 place = getPlace(m_time);
    const float ground = m_terrain.getHeight(place.x, place.y).value_or(0.0f);
    GC_INFO("Flythrough {:3.0f} s: {:4} frames, {:5.2f} ms average, {:5.2f} ms worst. At ({:5.0f}, {:5.0f}), {:5.0f} m up. {:4} patches ({:.2f} M "
            "triangles), {:4} tiles in the cache, {:3} uploads, at most {:3} tiles missing",
            m_time, m_second_frame_times.size(), average, worst, place.x, place.y, place.z - ground, stats.patches,
            static_cast<double>(stats.triangles) / 1.0e6, stats.resident, m_second_uploads, m_second_most_missing);
    m_second_frame_times.clear();
    m_second_uploads = 0;
    m_second_most_missing = 0;
}

void FlythroughSystem::logSummary()
{
    if (m_frame_times.empty()) {
        return;
    }
    std::vector<float> sorted = m_frame_times;
    std::sort(sorted.begin(), sorted.end());
    const float average = std::accumulate(sorted.begin(), sorted.end(), 0.0f) / static_cast<float>(sorted.size());
    const float p99 = sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)];
    const gc::TerrainSystem::Stats& stats = m_terrain_system.getStats();
    GC_INFO("Flythrough done: {} frames in {:.1f} s. Frame time {:.2f} ms average ({:.0f} fps), {:.2f} ms 99th percentile, {:.2f} ms worst", sorted.size(),
            m_time, average, 1000.0f / average, p99, sorted.back());
    GC_INFO("Flythrough done: at most {} patches ({:.2f} M triangles) and {} tiles in the cache at once. {} tiles ({}) uploaded. At most {} needed tiles "
            "were missing in a frame",
            m_most_patches, static_cast<double>(m_most_triangles) / 1.0e6, m_most_slots_used, stats.total_uploads,
            gc::bytesToHumanReadable(stats.total_upload_bytes), m_most_missing);
}
