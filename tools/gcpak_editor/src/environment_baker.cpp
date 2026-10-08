#include "environment_baker.h"

#include <cmath>
#include <cstdint>

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <memory>
#include <thread>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include <stb_image.h>

#include <gclog/gclog.h>

#include <gamecore/gc_asset_data.h>
#include <gamecore/gc_resources.h>
#include <gamecore/gc_stopwatch.h>

namespace {

constexpr float PI = 3.14159265358979323846f;

// The sky that is seen has about as many texels as the panorama does (a face covers a quarter of the way round), up to this
constexpr uint32_t MAX_SKY_FACE_SIZE = 2048;
// The sky that things are lit by. Everything that is worked out from it is blurry, so it can be small
constexpr uint32_t LIGHTING_FACE_SIZE = 256;
constexpr uint32_t IRRADIANCE_FACE_SIZE = 32;
// The specular cube map's first level is the lighting sky as it is (a mirror), and each level after it is for a rougher
// surface: roughness = level / (SPECULAR_LEVELS - 1). The shader has to pick the level the same way.
constexpr uint32_t SPECULAR_LEVELS = 6; // 256, 128, 64, 32, 16, 8
constexpr uint32_t SPECULAR_SAMPLES = 128;

// A texel is part of the sun (or of its glare) if it is this many times brighter than nearly all of the sky is: than the
// luminance that 99% of the sky (by area) is below
constexpr float SUN_THRESHOLD = 8.0f;

float luminance(const glm::vec3& color) { return glm::dot(color, glm::vec3{0.2126f, 0.7152f, 0.0722f}); }

// Runs fn(0) ... fn(count - 1) on every core
template <typename F>
void parallelFor(uint32_t count, F&& fn)
{
    const uint32_t thread_count = std::clamp(std::thread::hardware_concurrency(), 1u, 32u);
    std::atomic<uint32_t> next{0};
    const auto work = [&]() {
        for (uint32_t i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
            fn(i);
        }
    };
    std::vector<std::thread> threads{};
    for (uint32_t i = 1; i < thread_count; ++i) {
        threads.emplace_back(work);
    }
    work();
    for (std::thread& thread : threads) {
        thread.join();
    }
}

//
// Cube maps. The faces are in Vulkan's order (+X -X +Y -Y +Z -Z) and are laid out as Vulkan samples them, so a direction in the
// engine's space is what the shaders sample with.
//

constexpr uint32_t FACE_COUNT = 6;

// The direction of a point on a face. s and t are 0 to 1 across the face. Not normalised
glm::vec3 cubeDirection(uint32_t face, float s, float t)
{
    const float sc = 2.0f * s - 1.0f;
    const float tc = 2.0f * t - 1.0f;
    switch (face) {
    case 0:
        return {1.0f, -tc, -sc};
    case 1:
        return {-1.0f, -tc, sc};
    case 2:
        return {sc, 1.0f, tc};
    case 3:
        return {sc, -1.0f, -tc};
    case 4:
        return {sc, -tc, 1.0f};
    default:
        return {-sc, -tc, -1.0f};
    }
}

// The opposite of cubeDirection()
void directionToCube(const glm::vec3& d, uint32_t& face, float& s, float& t)
{
    const glm::vec3 a = glm::abs(d);
    float sc{}, tc{}, major{};
    if (a.x >= a.y && a.x >= a.z) {
        major = a.x;
        face = (d.x > 0.0f) ? 0 : 1;
        sc = (d.x > 0.0f) ? -d.z : d.z;
        tc = -d.y;
    }
    else if (a.y >= a.z) {
        major = a.y;
        face = (d.y > 0.0f) ? 2 : 3;
        sc = d.x;
        tc = (d.y > 0.0f) ? d.z : -d.z;
    }
    else {
        major = a.z;
        face = (d.z > 0.0f) ? 4 : 5;
        sc = (d.z > 0.0f) ? d.x : -d.x;
        tc = -d.y;
    }
    s = 0.5f * (sc / major + 1.0f);
    t = 0.5f * (tc / major + 1.0f);
}

// How much of the sphere a texel covers, in steradians. Texels at the corners of a face cover less than those in the middle
float texelSolidAngle(uint32_t size, uint32_t x, uint32_t y)
{
    const auto area = [](float u, float v) { return std::atan2(u * v, std::sqrt(u * u + v * v + 1.0f)); };
    const float inverse_size = 1.0f / static_cast<float>(size);
    const float u = 2.0f * (static_cast<float>(x) + 0.5f) * inverse_size - 1.0f;
    const float v = 2.0f * (static_cast<float>(y) + 0.5f) * inverse_size - 1.0f;
    return area(u - inverse_size, v - inverse_size) - area(u - inverse_size, v + inverse_size) - area(u + inverse_size, v - inverse_size) +
           area(u + inverse_size, v + inverse_size);
}

struct CubeLevel {
    uint32_t size{};
    std::vector<glm::vec3> texels{}; // 6 faces of size * size

    explicit CubeLevel(uint32_t size) : size(size), texels(static_cast<size_t>(FACE_COUNT) * size * size) {}

    glm::vec3& at(uint32_t face, uint32_t x, uint32_t y) { return texels[(static_cast<size_t>(face) * size + y) * size + x]; }
    const glm::vec3& at(uint32_t face, uint32_t x, uint32_t y) const { return texels[(static_cast<size_t>(face) * size + y) * size + x]; }

    // A texel that may be off the edge of its face, in which case it is the texel of the face next door that is there
    const glm::vec3& fetch(uint32_t face, int x, int y) const
    {
        const int limit = static_cast<int>(size);
        if (x >= 0 && y >= 0 && x < limit && y < limit) {
            return at(face, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
        }
        const glm::vec3 direction = cubeDirection(face, (static_cast<float>(x) + 0.5f) / static_cast<float>(size),
                                                  (static_cast<float>(y) + 0.5f) / static_cast<float>(size));
        uint32_t other_face{};
        float s{}, t{};
        directionToCube(direction, other_face, s, t);
        return at(other_face, static_cast<uint32_t>(std::clamp(static_cast<int>(s * static_cast<float>(size)), 0, limit - 1)),
                  static_cast<uint32_t>(std::clamp(static_cast<int>(t * static_cast<float>(size)), 0, limit - 1)));
    }

    // Bilinear, across the edges of the faces
    glm::vec3 sample(const glm::vec3& direction) const
    {
        uint32_t face{};
        float s{}, t{};
        directionToCube(direction, face, s, t);
        const float fx = s * static_cast<float>(size) - 0.5f;
        const float fy = t * static_cast<float>(size) - 0.5f;
        const int x = static_cast<int>(std::floor(fx));
        const int y = static_cast<int>(std::floor(fy));
        const float wx = fx - static_cast<float>(x);
        const float wy = fy - static_cast<float>(y);
        return glm::mix(glm::mix(fetch(face, x, y), fetch(face, x + 1, y), wx), glm::mix(fetch(face, x, y + 1), fetch(face, x + 1, y + 1), wx), wy);
    }
};

// A cube map and smaller and smaller copies of it, down to one texel a face
struct CubeMips {
    std::vector<CubeLevel> levels{};

    explicit CubeMips(CubeLevel&& first)
    {
        levels.push_back(std::move(first));
        while (levels.back().size > 1) {
            const CubeLevel& source = levels.back();
            CubeLevel smaller(source.size / 2);
            for (uint32_t face = 0; face < FACE_COUNT; ++face) {
                for (uint32_t y = 0; y < smaller.size; ++y) {
                    for (uint32_t x = 0; x < smaller.size; ++x) {
                        smaller.at(face, x, y) = 0.25f * (source.at(face, 2 * x, 2 * y) + source.at(face, 2 * x + 1, 2 * y) + source.at(face, 2 * x, 2 * y + 1) +
                                                          source.at(face, 2 * x + 1, 2 * y + 1));
                    }
                }
            }
            levels.push_back(std::move(smaller));
        }
    }

    // Trilinear. Level 0 is the full size
    glm::vec3 sample(const glm::vec3& direction, float level) const
    {
        level = std::clamp(level, 0.0f, static_cast<float>(levels.size() - 1));
        const size_t lower = static_cast<size_t>(level);
        const size_t upper = std::min(lower + 1, levels.size() - 1);
        return glm::mix(levels[lower].sample(direction), levels[upper].sample(direction), level - static_cast<float>(lower));
    }
};

//
// The panorama: every direction, with longitude across the image and latitude down it. +Y is up, as in glTF, so directions are
// turned to the engine's Z-up in the same way that glTF scenes are: (x, y, z) becomes (x, -z, y).
//

struct Panorama {
    uint32_t width{};
    uint32_t height{};
    const float* rgb{}; // rows from the top

    glm::vec3 texel(uint32_t x, uint32_t y) const
    {
        const float* const p = rgb + (static_cast<size_t>(y) * width + x) * 3;
        // (a file can have anything in it. Nothing is darker than black, and a NaN isn't greater than zero)
        return {p[0] > 0.0f ? p[0] : 0.0f, p[1] > 0.0f ? p[1] : 0.0f, p[2] > 0.0f ? p[2] : 0.0f};
    }

    // The direction (in the engine's space) of the middle of a texel
    glm::vec3 direction(float sin_theta, float cos_theta, float sin_phi, float cos_phi) const
    {
        return {sin_theta * cos_phi, -sin_theta * sin_phi, cos_theta};
    }

    // Bilinear. The image wraps round sideways
    glm::vec3 sample(const glm::vec3& d) const
    {
        const float u = std::atan2(-d.y, d.x) / (2.0f * PI) + 0.5f;
        const float v = std::acos(std::clamp(d.z, -1.0f, 1.0f)) / PI;
        const float fx = u * static_cast<float>(width) - 0.5f;
        const float fy = std::clamp(v * static_cast<float>(height) - 0.5f, 0.0f, static_cast<float>(height - 1));
        const int x = static_cast<int>(std::floor(fx));
        const uint32_t y0 = static_cast<uint32_t>(fy);
        const uint32_t y1 = std::min(y0 + 1, height - 1);
        const float wx = fx - static_cast<float>(x);
        const float wy = fy - static_cast<float>(y0);
        const uint32_t x0 = static_cast<uint32_t>((x + static_cast<int>(width)) % static_cast<int>(width));
        const uint32_t x1 = (x0 + 1) % width;
        return glm::mix(glm::mix(texel(x0, y0), texel(x1, y0), wx), glm::mix(texel(x0, y1), texel(x1, y1), wx), wy);
    }
};

// The angles of the panorama's columns and rows
struct PanoramaAngles {
    std::vector<float> sin_phi{}, cos_phi{}, sin_theta{}, cos_theta{};

    explicit PanoramaAngles(const Panorama& panorama)
        : sin_phi(panorama.width), cos_phi(panorama.width), sin_theta(panorama.height), cos_theta(panorama.height)
    {
        for (uint32_t x = 0; x < panorama.width; ++x) {
            const float phi = ((static_cast<float>(x) + 0.5f) / static_cast<float>(panorama.width) - 0.5f) * 2.0f * PI;
            sin_phi[x] = std::sin(phi);
            cos_phi[x] = std::cos(phi);
        }
        for (uint32_t y = 0; y < panorama.height; ++y) {
            const float theta = (static_cast<float>(y) + 0.5f) / static_cast<float>(panorama.height) * PI;
            sin_theta[y] = std::sin(theta);
            cos_theta[y] = std::cos(theta);
        }
    }
};

// The luminance that 99% of the panorama, by area, is below
float getTypicalBrightLuminance(const Panorama& panorama, const PanoramaAngles& angles)
{
    // a histogram of the logarithm of the luminance, with 8 bins for every doubling
    constexpr float MIN_LOG = -24.0f;
    constexpr float BINS_PER_STOP = 8.0f;
    std::array<double, 384> histogram{};
    double total = 0.0;
    for (uint32_t y = 0; y < panorama.height; ++y) {
        const double weight = angles.sin_theta[y]; // rows near the poles cover less of the sphere
        for (uint32_t x = 0; x < panorama.width; ++x) {
            const float l = luminance(panorama.texel(x, y));
            const float bin = (l > 0.0f) ? (std::log2(l) - MIN_LOG) * BINS_PER_STOP : 0.0f;
            histogram[static_cast<size_t>(std::clamp(bin, 0.0f, static_cast<float>(histogram.size() - 1)))] += weight;
        }
        total += weight * panorama.width;
    }
    double below = 0.0;
    for (size_t bin = 0; bin < histogram.size(); ++bin) {
        below += histogram[bin];
        if (below >= 0.99 * total) {
            return std::exp2((static_cast<float>(bin) + 1.0f) / BINS_PER_STOP + MIN_LOG);
        }
    }
    return std::numeric_limits<float>::max();
}

struct LightingSky {
    CubeLevel cube{LIGHTING_FACE_SIZE};
    gc::CubeMapInfo info{}; // what was found out about the sky and its sun (the sizes aren't set)
};

// Makes the sky that things are lit by: the panorama without the sun, at a size where every texel is the average of the part of
// the panorama that it covers.
LightingSky makeLightingSky(const Panorama& panorama, const PanoramaAngles& angles)
{
    LightingSky sky{};
    const float sun_luminance = SUN_THRESHOLD * getTypicalBrightLuminance(panorama, angles);

    std::vector<float> weights(sky.cube.texels.size(), 0.0f);
    glm::dvec3 sun_direction{0.0};
    double sun_illuminance = 0.0;
    const float texel_angles = (PI / static_cast<float>(panorama.height)) * (2.0f * PI / static_cast<float>(panorama.width));
    for (uint32_t y = 0; y < panorama.height; ++y) {
        const float solid_angle = angles.sin_theta[y] * texel_angles;
        for (uint32_t x = 0; x < panorama.width; ++x) {
            glm::vec3 color = panorama.texel(x, y);
            const glm::vec3 direction = panorama.direction(angles.sin_theta[y], angles.cos_theta[y], angles.sin_phi[x], angles.cos_phi[x]);

            if (const float l = luminance(color); l > sun_luminance) {
                // part of the sun. What is taken off it is what a directional light should give instead
                const double excess = static_cast<double>(l - sun_luminance) * solid_angle;
                sun_illuminance += excess;
                sun_direction += glm::dvec3(direction) * excess;
                color *= sun_luminance / l;
            }

            uint32_t face{};
            float s{}, t{};
            directionToCube(direction, face, s, t);
            const uint32_t cube_x = std::min(static_cast<uint32_t>(s * LIGHTING_FACE_SIZE), LIGHTING_FACE_SIZE - 1);
            const uint32_t cube_y = std::min(static_cast<uint32_t>(t * LIGHTING_FACE_SIZE), LIGHTING_FACE_SIZE - 1);
            sky.cube.at(face, cube_x, cube_y) += color * solid_angle;
            weights[(static_cast<size_t>(face) * LIGHTING_FACE_SIZE + cube_y) * LIGHTING_FACE_SIZE + cube_x] += solid_angle;
        }
    }

    double sky_illuminance = 0.0;
    for (uint32_t face = 0; face < FACE_COUNT; ++face) {
        for (uint32_t y = 0; y < LIGHTING_FACE_SIZE; ++y) {
            for (uint32_t x = 0; x < LIGHTING_FACE_SIZE; ++x) {
                glm::vec3& texel = sky.cube.at(face, x, y);
                const glm::vec3 direction =
                    glm::normalize(cubeDirection(face, (static_cast<float>(x) + 0.5f) / LIGHTING_FACE_SIZE, (static_cast<float>(y) + 0.5f) / LIGHTING_FACE_SIZE));
                const float weight = weights[(static_cast<size_t>(face) * LIGHTING_FACE_SIZE + y) * LIGHTING_FACE_SIZE + x];
                if (weight > 0.0f) {
                    texel /= weight;
                }
                else {
                    // a small panorama, with fewer texels than the cube map
                    texel = panorama.sample(direction);
                    if (const float l = luminance(texel); l > sun_luminance) {
                        texel *= sun_luminance / l;
                    }
                }
                // what a surface facing up gets from this texel
                sky_illuminance += static_cast<double>(luminance(texel)) * std::max(direction.z, 0.0f) * texelSolidAngle(LIGHTING_FACE_SIZE, x, y);
            }
        }
    }

    sky.info.sky_illuminance = static_cast<float>(sky_illuminance);
    sky.info.sun_illuminance = static_cast<float>(sun_illuminance);
    if (sun_illuminance > 0.0 && glm::dot(sun_direction, sun_direction) > 0.0) {
        sky.info.sun_direction = glm::vec3(glm::normalize(sun_direction));
    }
    return sky;
}

// The light that reaches a surface facing each direction, from every texel of the sky above it (each counting for less the
// nearer it is to the surface's horizon). It is divided by pi, so that a texel is the luminance of a white diffuse surface.
CubeLevel makeIrradiance(const CubeMips& sky)
{
    // The result has no detail in it, so a small copy of the sky is enough to work it out from
    const CubeLevel* source = &sky.levels.back();
    for (const CubeLevel& level : sky.levels) {
        if (level.size <= IRRADIANCE_FACE_SIZE) {
            source = &level;
            break;
        }
    }
    struct SourceTexel {
        glm::vec3 direction;
        glm::vec3 light; // radiance times solid angle
    };
    std::vector<SourceTexel> source_texels{};
    source_texels.reserve(source->texels.size());
    for (uint32_t face = 0; face < FACE_COUNT; ++face) {
        for (uint32_t y = 0; y < source->size; ++y) {
            for (uint32_t x = 0; x < source->size; ++x) {
                const float inverse_size = 1.0f / static_cast<float>(source->size);
                source_texels.push_back(
                    SourceTexel{glm::normalize(cubeDirection(face, (static_cast<float>(x) + 0.5f) * inverse_size, (static_cast<float>(y) + 0.5f) * inverse_size)),
                                source->at(face, x, y) * texelSolidAngle(source->size, x, y)});
            }
        }
    }

    CubeLevel irradiance(IRRADIANCE_FACE_SIZE);
    parallelFor(FACE_COUNT * IRRADIANCE_FACE_SIZE, [&](uint32_t row) {
        const uint32_t face = row / IRRADIANCE_FACE_SIZE;
        const uint32_t y = row % IRRADIANCE_FACE_SIZE;
        for (uint32_t x = 0; x < IRRADIANCE_FACE_SIZE; ++x) {
            const glm::vec3 normal =
                glm::normalize(cubeDirection(face, (static_cast<float>(x) + 0.5f) / IRRADIANCE_FACE_SIZE, (static_cast<float>(y) + 0.5f) / IRRADIANCE_FACE_SIZE));
            glm::vec3 sum{0.0f};
            for (const SourceTexel& texel : source_texels) {
                sum += texel.light * std::max(glm::dot(normal, texel.direction), 0.0f);
            }
            irradiance.at(face, x, y) = sum / PI;
        }
    });
    return irradiance;
}

// What a surface of some roughness reflects towards someone looking straight at it (which is what has to be assumed, to have
// one answer for each direction): the sky around the mirror direction, blurred by the GGX distribution that the engine's shader
// uses for lights. This is the first half of the "split sum" of Karis, "Real Shading in Unreal Engine 4" (2013).
CubeLevel makeSpecularLevel(const CubeMips& sky, uint32_t size, float roughness)
{
    const float alpha = roughness * roughness;
    const float alpha_squared = alpha * alpha;
    const float sky_texel_solid_angle = 4.0f * PI / (static_cast<float>(FACE_COUNT) * static_cast<float>(sky.levels[0].size) * static_cast<float>(sky.levels[0].size));

    // The half vectors to sample (around +Z), most of them where the distribution is highest, and what each one needs
    struct Sample {
        glm::vec3 light; // the direction that is reflected towards the viewer, if the surface faces +Z
        float level;     // how blurry a copy of the sky to read: one whose texels are as big as what the sample stands for
    };
    std::vector<Sample> samples{};
    for (uint32_t i = 0; i < SPECULAR_SAMPLES; ++i) {
        // the Hammersley sequence: evenly spread points in a square
        uint32_t bits = i;
        bits = (bits << 16) | (bits >> 16);
        bits = ((bits & 0x55555555u) << 1) | ((bits & 0xaaaaaaaau) >> 1);
        bits = ((bits & 0x33333333u) << 2) | ((bits & 0xccccccccu) >> 2);
        bits = ((bits & 0x0f0f0f0fu) << 4) | ((bits & 0xf0f0f0f0u) >> 4);
        bits = ((bits & 0x00ff00ffu) << 8) | ((bits & 0xff00ff00u) >> 8);
        const float u1 = (static_cast<float>(i) + 0.5f) / static_cast<float>(SPECULAR_SAMPLES);
        const float u2 = static_cast<float>(bits) * 2.3283064365386963e-10f;

        const float phi = 2.0f * PI * u2;
        const float cos_theta = std::sqrt((1.0f - u1) / (1.0f + (alpha_squared - 1.0f) * u1));
        const float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
        const glm::vec3 half{sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta};
        // the view direction is +Z
        const glm::vec3 light = 2.0f * cos_theta * half - glm::vec3{0.0f, 0.0f, 1.0f};
        if (light.z <= 0.0f) {
            continue;
        }
        const float denominator = cos_theta * cos_theta * (alpha_squared - 1.0f) + 1.0f;
        const float distribution = alpha_squared / (PI * denominator * denominator);
        const float pdf = distribution / 4.0f;
        const float sample_solid_angle = 1.0f / (static_cast<float>(SPECULAR_SAMPLES) * pdf);
        samples.push_back(Sample{light, std::max(0.5f * std::log2(sample_solid_angle / sky_texel_solid_angle) + 1.0f, 0.0f)});
    }

    CubeLevel result(size);
    parallelFor(FACE_COUNT * size, [&](uint32_t row) {
        const uint32_t face = row / size;
        const uint32_t y = row % size;
        for (uint32_t x = 0; x < size; ++x) {
            const glm::vec3 normal = glm::normalize(cubeDirection(face, (static_cast<float>(x) + 0.5f) / static_cast<float>(size),
                                                                  (static_cast<float>(y) + 0.5f) / static_cast<float>(size)));
            const glm::vec3 other = (std::abs(normal.z) < 0.999f) ? glm::vec3{0.0f, 0.0f, 1.0f} : glm::vec3{1.0f, 0.0f, 0.0f};
            const glm::vec3 tangent = glm::normalize(glm::cross(other, normal));
            const glm::vec3 bitangent = glm::cross(normal, tangent);

            glm::vec3 sum{0.0f};
            float total_weight = 0.0f;
            for (const Sample& sample : samples) {
                const glm::vec3 direction = tangent * sample.light.x + bitangent * sample.light.y + normal * sample.light.z;
                sum += sky.sample(direction, sample.level) * sample.light.z;
                total_weight += sample.light.z;
            }
            result.at(face, x, y) = (total_weight > 0.0f) ? sum / total_weight : sky.levels[0].sample(normal);
        }
    });
    return result;
}

// The sky that is seen: the panorama as it is
CubeLevel makeVisibleSky(const Panorama& panorama, uint32_t size)
{
    CubeLevel sky(size);
    parallelFor(FACE_COUNT * size, [&](uint32_t row) {
        const uint32_t face = row / size;
        const uint32_t y = row % size;
        for (uint32_t x = 0; x < size; ++x) {
            sky.at(face, x, y) = panorama.sample(glm::normalize(
                cubeDirection(face, (static_cast<float>(x) + 0.5f) / static_cast<float>(size), (static_cast<float>(y) + 0.5f) / static_cast<float>(size))));
        }
    });
    return sky;
}

// Vulkan's E5B9G9R9_UFLOAT_PACK32: three 9 bit numbers that share a 5 bit exponent. Half the size of 16 bit floats, and bright
// enough for anything but the middle of the sun (which is clamped). From the specification of EXT_texture_shared_exponent.
uint32_t packSharedExponent(const glm::vec3& color)
{
    constexpr float MAX_VALUE = 65408.0f; // (2^9 - 1) / 2^9 * 2^(31 - 15)
    const float r = std::clamp(color.r, 0.0f, MAX_VALUE);
    const float g = std::clamp(color.g, 0.0f, MAX_VALUE);
    const float b = std::clamp(color.b, 0.0f, MAX_VALUE);
    const float max_component = std::max(r, std::max(g, b));
    if (!(max_component > 0.0f)) {
        return 0;
    }
    int exponent = std::max(-16, static_cast<int>(std::floor(std::log2(max_component)))) + 1 + 15;
    float scale = std::exp2(static_cast<float>(exponent - 15 - 9));
    if (static_cast<int>(std::floor(max_component / scale + 0.5f)) == 512) {
        exponent += 1;
        scale *= 2.0f;
    }
    const auto mantissa = [scale](float value) { return static_cast<uint32_t>(std::floor(value / scale + 0.5f)); };
    return (static_cast<uint32_t>(exponent) << 27) | (mantissa(b) << 18) | (mantissa(g) << 9) | mantissa(r);
}

// The data of a cube map asset, from its mip levels, largest first
std::vector<uint8_t> makeCubeMapAssetData(gc::CubeMapInfo info, std::span<const CubeLevel> levels)
{
    info.face_size = levels[0].size;
    info.mip_count = static_cast<uint32_t>(levels.size());
    std::vector<uint32_t> texels{};
    for (const CubeLevel& level : levels) {
        const size_t offset = texels.size();
        texels.resize(offset + level.texels.size());
        parallelFor(FACE_COUNT * level.size, [&](uint32_t row) {
            const size_t first = static_cast<size_t>(row) * level.size;
            for (size_t i = first; i < first + level.size; ++i) {
                texels[offset + i] = packSharedExponent(level.texels[i]);
            }
        });
    }
    return gc::makeCubeMapData(info, texels);
}

uint32_t floorPowerOfTwo(uint32_t value)
{
    uint32_t result = 1;
    while (result * 2 <= value) {
        result *= 2;
    }
    return result;
}

} // namespace

bool compileEnvironment(const std::filesystem::path& path, std::vector<Asset>& assets_out)
{
    const std::string name = path.filename().string();
    const gc::Stopwatch stopwatch{};

    int width{}, height{}, channels_in_file{};
    const std::unique_ptr<float, decltype(&stbi_image_free)> pixels(stbi_loadf(path.string().c_str(), &width, &height, &channels_in_file, 3),
                                                                    stbi_image_free);
    if (!pixels || width < 8 || height < 4) {
        GC_ERROR("Failed to read image {}: {}", name, pixels ? "it is too small" : stbi_failure_reason());
        return false;
    }
    if (width != 2 * height) {
        GC_WARN("{}: is {}x{}. A panorama of the whole sky is twice as wide as it is high, so this one will look stretched", name, width, height);
    }
    const Panorama panorama{static_cast<uint32_t>(width), static_cast<uint32_t>(height), pixels.get()};
    const PanoramaAngles angles(panorama);

    const uint32_t sky_size = std::min(floorPowerOfTwo(panorama.width / 4), MAX_SKY_FACE_SIZE);
    const CubeLevel visible_sky = makeVisibleSky(panorama, sky_size);

    LightingSky lighting = makeLightingSky(panorama, angles);
    const gc::CubeMapInfo info = lighting.info;
    const CubeMips lighting_mips(std::move(lighting.cube));

    const CubeLevel irradiance = makeIrradiance(lighting_mips);

    std::vector<CubeLevel> specular{};
    specular.push_back(lighting_mips.levels[0]); // a mirror reflects the sky as it is
    for (uint32_t level = 1; level < SPECULAR_LEVELS; ++level) {
        specular.push_back(makeSpecularLevel(lighting_mips, LIGHTING_FACE_SIZE >> level, static_cast<float>(level) / static_cast<float>(SPECULAR_LEVELS - 1)));
    }

    // (only the sky's own asset says where the sun is)
    assets_out.push_back(makeAsset(name, makeCubeMapAssetData(info, std::span(&visible_sky, 1)), gcpak::GcpakAssetType::CUBE_MAP_E5B9G9R9));
    assets_out.push_back(
        makeAsset(std::format("{}/irradiance", name), makeCubeMapAssetData({}, std::span(&irradiance, 1)), gcpak::GcpakAssetType::CUBE_MAP_E5B9G9R9));
    assets_out.push_back(makeAsset(std::format("{}/specular", name), makeCubeMapAssetData({}, specular), gcpak::GcpakAssetType::CUBE_MAP_E5B9G9R9));

    GC_INFO("{}: baked a {}x{} sky, its irradiance ({}x{}) and its reflections ({}x{}, {} levels) from a {}x{} panorama in {}", name, sky_size, sky_size,
            IRRADIANCE_FACE_SIZE, IRRADIANCE_FACE_SIZE, LIGHTING_FACE_SIZE, LIGHTING_FACE_SIZE, SPECULAR_LEVELS, width, height, stopwatch);
    if (info.sun_illuminance > 0.0f) {
        GC_INFO("{}: the sun is {:.1f} degrees above the horizon, at ({:.3f}, {:.3f}, {:.3f}), and gives {:.1f} times the light that the rest of the sky "
                "does. It is left out of the sky's light: use a directional light for it",
                name, glm::degrees(std::asin(std::clamp(info.sun_direction.z, -1.0f, 1.0f))), info.sun_direction.x, info.sun_direction.y,
                info.sun_direction.z, info.sun_illuminance / std::max(info.sky_illuminance, 1.0e-20f));
    }
    else {
        GC_INFO("{}: has no sun in it (nothing is much brighter than the rest of the sky)", name);
    }
    return true;
}
