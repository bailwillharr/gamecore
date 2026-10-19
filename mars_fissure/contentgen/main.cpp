//
// mars_fissure_contentgen.exe OUT.gcpak SKY.hdr
//
// Makes the small things that Mars Fissure needs besides its terrain (which gcpak_editor bakes from the DTM, see README):
//  - the ground's material, whose textures are tiled across the terrain: dust and pebbles, and their bumps. Saved to OUT.gcpak
//  - a panorama of a Martian sky, saved as a .hdr file, which gcpak_editor then turns into the sky's cube maps
// Both are made by code, as there are no source images for them.
//

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <gcpak/gcpak.h>

#include <gamecore/gc_asset_data.h>
#include <gamecore/gc_name.h>
#include <gamecore/gc_resources.h>

#include "content_names.h"

template <typename... Args>
static void print(std::format_string<Args...> fmt, Args&&... args)
{
    const std::string line = std::format("[mars_fissure_contentgen] {}\n", std::format(fmt, std::forward<Args>(args)...));
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
}

//
// Noise that tiles: the lattice repeats every 'period' cells
//

static float hash(int x, int y, int seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + static_cast<uint32_t>(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xffffff) / static_cast<float>(0xffffff);
}

// u and v from 0 to 1 cover the texture once
static float valueNoise(float u, float v, int period, int seed)
{
    const float x = u * static_cast<float>(period);
    const float y = v * static_cast<float>(period);
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    float fx = x - static_cast<float>(x0);
    float fy = y - static_cast<float>(y0);
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    const auto corner = [&](int cx, int cy) { return hash(((cx % period) + period) % period, ((cy % period) + period) % period, seed); };
    return std::lerp(std::lerp(corner(x0, y0), corner(x0 + 1, y0), fx), std::lerp(corner(x0, y0 + 1), corner(x0 + 1, y0 + 1), fx), fy);
}

static float fbm(float u, float v, int period, int octaves, int seed)
{
    float sum = 0.0f;
    float amplitude = 0.5f;
    float total = 0.0f;
    for (int octave = 0; octave < octaves; ++octave) {
        sum += amplitude * valueNoise(u, v, period << octave, seed + octave);
        total += amplitude;
        amplitude *= 0.5f;
    }
    return sum / total;
}

static uint8_t toByte(float value) { return static_cast<uint8_t>(std::clamp(std::round(value * 255.0f), 0.0f, 255.0f)); }

static float linearToSrgb(float c) { return (c <= 0.0031308f) ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

//
// The ground: dust with some pebbles in it. A texture covers a few metres (see terrain.frag)
//

static constexpr uint32_t GROUND_SIZE = 512;

struct Pebble {
    float u, v, radius; // in texels
    float shade;
};

static std::vector<Pebble> makePebbles()
{
    std::mt19937 random{4242};
    std::uniform_real_distribution<float> position(0.0f, static_cast<float>(GROUND_SIZE));
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::vector<Pebble> pebbles{};
    for (int i = 0; i < 220; ++i) {
        // mostly small ones
        const float size = unit(random);
        pebbles.push_back(Pebble{position(random), position(random), 1.5f + 6.0f * size * size * size, unit(random)});
    }
    return pebbles;
}

// How much of a pebble covers a texel (0 to 1), and how high it is there (a dome, 0 to 1). The texture wraps
static void samplePebbles(const std::vector<Pebble>& pebbles, float x, float y, float& cover, float& height, float& shade)
{
    cover = 0.0f;
    height = 0.0f;
    shade = 0.0f;
    const float size = static_cast<float>(GROUND_SIZE);
    for (const Pebble& pebble : pebbles) {
        float dx = std::abs(x - pebble.u);
        float dy = std::abs(y - pebble.v);
        dx = std::min(dx, size - dx);
        dy = std::min(dy, size - dy);
        const float d = std::sqrt(dx * dx + dy * dy) / pebble.radius;
        if (d < 1.0f) {
            const float dome = std::sqrt(1.0f - d * d);
            if (dome > height) {
                height = dome;
                shade = pebble.shade;
            }
            cover = std::max(cover, std::clamp((1.0f - d) * pebble.radius, 0.0f, 1.0f)); // (a soft edge, a texel wide)
        }
    }
}

static void makeGround(std::vector<gcpak::GcpakCreator::Asset>& assets)
{
    const std::vector<Pebble> pebbles = makePebbles();
    std::vector<float> heights(GROUND_SIZE * GROUND_SIZE);
    std::vector<uint8_t> albedo(GROUND_SIZE * GROUND_SIZE * 4);

    // Martian dust is a dull orange-brown, about a fifth of the light it gets is reflected. The pebbles are darker basalt
    const glm::vec3 dust{0.376f, 0.127f, 0.053f};
    const glm::vec3 basalt{0.16f, 0.09f, 0.06f};
    for (uint32_t y = 0; y < GROUND_SIZE; ++y) {
        for (uint32_t x = 0; x < GROUND_SIZE; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(GROUND_SIZE);
            const float v = static_cast<float>(y) / static_cast<float>(GROUND_SIZE);
            const float blotches = fbm(u, v, 4, 4, 1);
            const float grain = valueNoise(u, v, 256, 7);
            const float ripples = 0.5f + 0.5f * std::sin(2.0f * std::numbers::pi_v<float> * (u * 3.0f + v * 5.0f) + 6.0f * fbm(u, v, 8, 3, 3));
            float cover{}, dome{}, shade{};
            samplePebbles(pebbles, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, cover, dome, shade);

            glm::vec3 color = dust * (0.75f + 0.45f * blotches + 0.2f * (grain - 0.5f));
            // (dusty, so not much darker than the ground, and lighter on top)
            color = glm::mix(color, glm::mix(basalt, dust * 0.75f, shade) * (0.8f + 0.25f * dome + 0.2f * grain), cover * 0.85f);
            const size_t i = (static_cast<size_t>(y) * GROUND_SIZE + x) * 4;
            albedo[i + 0] = toByte(linearToSrgb(color.r));
            albedo[i + 1] = toByte(linearToSrgb(color.g));
            albedo[i + 2] = toByte(linearToSrgb(color.b));
            albedo[i + 3] = 255;
            heights[static_cast<size_t>(y) * GROUND_SIZE + x] = 1.5f * fbm(u, v, 16, 4, 11) + 0.6f * ripples + 3.0f * dome * cover;
        }
    }

    // the bumps' normals, from the heights (which wrap)
    std::vector<uint8_t> normals(GROUND_SIZE * GROUND_SIZE * 4);
    const auto height_at = [&](int x, int y) {
        const int size = static_cast<int>(GROUND_SIZE);
        return heights[static_cast<size_t>(((y % size) + size) % size) * GROUND_SIZE + static_cast<size_t>(((x % size) + size) % size)];
    };
    for (int y = 0; y < static_cast<int>(GROUND_SIZE); ++y) {
        for (int x = 0; x < static_cast<int>(GROUND_SIZE); ++x) {
            const float dx = (height_at(x + 1, y) - height_at(x - 1, y)) * 0.5f;
            const float dy = (height_at(x, y + 1) - height_at(x, y - 1)) * 0.5f;
            const glm::vec3 n = glm::normalize(glm::vec3{-dx, -dy, 1.0f});
            const size_t i = (static_cast<size_t>(y) * GROUND_SIZE + static_cast<size_t>(x)) * 4;
            normals[i + 0] = toByte(n.x * 0.5f + 0.5f);
            normals[i + 1] = toByte(n.y * 0.5f + 0.5f);
            normals[i + 2] = toByte(n.z * 0.5f + 0.5f);
            normals[i + 3] = 255;
        }
    }

    assets.push_back({GROUND_ALBEDO, 0, gc::makeTextureData(GROUND_SIZE, GROUND_SIZE, albedo), gcpak::GcpakAssetType::TEXTURE_R8G8B8A8_SRGB});
    assets.push_back({GROUND_NORMAL, 0, gc::makeTextureData(GROUND_SIZE, GROUND_SIZE, normals), gcpak::GcpakAssetType::TEXTURE_R8G8B8A8});

    gc::ResourceMaterial material{};
    material.base_color_texture = gc::Name(GROUND_ALBEDO);
    material.normal_texture = gc::Name(GROUND_NORMAL);
    material.base_color = glm::vec4{1.0f, 1.0f, 1.0f, 1.0f}; // (tints the texture)
    material.roughness = 0.95f;
    material.metallic = 0.0f;
    assets.push_back({GROUND_MATERIAL, 0, gc::makeMaterialData(material), gcpak::GcpakAssetType::MATERIAL});
}

//
// The sky. Mars's air is thin, but full of fine dust, which makes the daytime sky a hazy butterscotch, darkest overhead, and
// scatters a pale blue glow around the sun. The panorama is in the engine's space (z up), as the environment baker reads it.
//

static constexpr uint32_t SKY_WIDTH = 2048;
static constexpr uint32_t SKY_HEIGHT = 1024;
static constexpr float SUN_ELEVATION_DEGREES = 32.0f; // the game's directional light is at the same height (see game.cpp)
static constexpr float SUN_RADIUS_DEGREES = 0.35f;    // (really about 0.18 from Mars, but that would be under a texel)
static constexpr float SUN_TO_SKY = 2.5f;             // how much more light comes straight from the sun than from the rest of the sky

static glm::vec3 skyColor(const glm::vec3& d, const glm::vec3& sun)
{
    const float elevation = std::clamp(d.z, -1.0f, 1.0f);
    const float up = std::max(elevation, 0.0f);
    const glm::vec3 zenith{0.30f, 0.18f, 0.105f};
    const glm::vec3 horizon{0.78f, 0.52f, 0.33f};
    glm::vec3 sky = glm::mix(horizon, zenith, std::pow(up, 0.6f));

    // Forward scattering by the dust around the sun (Henyey-Greenstein), bluer close in
    const float mu = glm::dot(d, sun);
    constexpr float G = 0.78f;
    const float phase = (1.0f - G * G) / std::pow(1.0f + G * G - 2.0f * G * mu, 1.5f) / (4.0f * std::numbers::pi_v<float>);
    const glm::vec3 glow_color = glm::mix(glm::vec3{0.95f, 0.75f, 0.55f}, glm::vec3{0.62f, 0.72f, 0.95f}, std::clamp((mu - 0.95f) / 0.05f, 0.0f, 1.0f));
    sky += glow_color * phase * 0.35f * (1.0f - 0.4f * up);
    // the sky towards the sun is brighter all the way down to the horizon
    sky *= 1.0f + 0.35f * std::max(mu, 0.0f);

    // Below the horizon: ground so far away that it is lost in the haze, so about the colour of the haze at the horizon, a little
    // darker further down. It is only seen past the edge of the terrain, which fades into it, and it lights the ground a little
    const glm::vec3 haze = horizon * (1.0f + 0.35f * std::max(mu, 0.0f));
    const glm::vec3 ground = haze * (0.85f - 0.35f * std::min(-elevation / 0.3f, 1.0f));
    const float into_ground = std::clamp(-elevation / 0.01f, 0.0f, 1.0f);
    return glm::mix(sky, ground, into_ground);
}

// Radiance .hdr, uncompressed RGBE scanlines from the top
static bool writeHdr(const std::filesystem::path& path, uint32_t width, uint32_t height, const std::vector<glm::vec3>& pixels)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " << height << " +X " << width << "\n";
    std::vector<uint8_t> row(static_cast<size_t>(width) * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const glm::vec3& c = pixels[static_cast<size_t>(y) * width + x];
            const float v = std::max({c.r, c.g, c.b});
            uint8_t* const out = &row[static_cast<size_t>(x) * 4];
            if (v < 1.0e-32f) {
                out[0] = out[1] = out[2] = out[3] = 0;
                continue;
            }
            int exponent{};
            const float mantissa = std::frexp(v, &exponent);
            const float scale = mantissa * 256.0f / v;
            out[0] = static_cast<uint8_t>(c.r * scale);
            out[1] = static_cast<uint8_t>(c.g * scale);
            out[2] = static_cast<uint8_t>(c.b * scale);
            out[3] = static_cast<uint8_t>(exponent + 128);
        }
        file.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
    return static_cast<bool>(file);
}

static bool makeSky(const std::filesystem::path& path)
{
    const float pi = std::numbers::pi_v<float>;
    const float elevation = SUN_ELEVATION_DEGREES * pi / 180.0f;
    const glm::vec3 sun{std::cos(elevation), 0.0f, std::sin(elevation)}; // (the game turns the sky to put its sun over the light)
    const float sun_radius = SUN_RADIUS_DEGREES * pi / 180.0f;
    const float cos_sun_radius = std::cos(sun_radius);

    // The light that the sky gives a surface facing up, to make the sun SUN_TO_SKY times that, on a surface facing it
    std::vector<glm::vec3> pixels(static_cast<size_t>(SKY_WIDTH) * SKY_HEIGHT);
    double sky_illuminance = 0.0;
    for (uint32_t y = 0; y < SKY_HEIGHT; ++y) {
        // (the same angles as the environment baker uses for each texel)
        const float theta = (static_cast<float>(y) + 0.5f) / static_cast<float>(SKY_HEIGHT) * pi;
        const float solid_angle = (2.0f * pi / static_cast<float>(SKY_WIDTH)) * (pi / static_cast<float>(SKY_HEIGHT)) * std::sin(theta);
        for (uint32_t x = 0; x < SKY_WIDTH; ++x) {
            const float phi = ((static_cast<float>(x) + 0.5f) / static_cast<float>(SKY_WIDTH) - 0.5f) * 2.0f * pi;
            const glm::vec3 d{std::sin(theta) * std::cos(phi), -std::sin(theta) * std::sin(phi), std::cos(theta)};
            const glm::vec3 color = skyColor(d, sun);
            pixels[static_cast<size_t>(y) * SKY_WIDTH + x] = color;
            if (d.z > 0.0f) {
                sky_illuminance += static_cast<double>(glm::dot(color, glm::vec3{0.2126f, 0.7152f, 0.0722f}) * d.z * solid_angle);
            }
        }
    }

    // The sun's disc
    const float sun_solid_angle = 2.0f * pi * (1.0f - cos_sun_radius);
    const float sun_luminance = SUN_TO_SKY * static_cast<float>(sky_illuminance) / sun_solid_angle;
    const glm::vec3 sun_color = glm::vec3{1.0f, 0.93f, 0.82f} * sun_luminance;
    for (uint32_t y = 0; y < SKY_HEIGHT; ++y) {
        const float theta = (static_cast<float>(y) + 0.5f) / static_cast<float>(SKY_HEIGHT) * pi;
        for (uint32_t x = 0; x < SKY_WIDTH; ++x) {
            const float phi = ((static_cast<float>(x) + 0.5f) / static_cast<float>(SKY_WIDTH) - 0.5f) * 2.0f * pi;
            const glm::vec3 d{std::sin(theta) * std::cos(phi), -std::sin(theta) * std::sin(phi), std::cos(theta)};
            if (glm::dot(d, sun) > std::cos(sun_radius * 2.5f)) {
                // a texel is about the size of the disc, so it is sampled finely to give its edge texels the right brightness
                int inside = 0;
                constexpr int SAMPLES = 8;
                for (int sy = 0; sy < SAMPLES; ++sy) {
                    for (int sx = 0; sx < SAMPLES; ++sx) {
                        const float t = (static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / SAMPLES) / static_cast<float>(SKY_HEIGHT) * pi;
                        const float p = ((static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / SAMPLES) / static_cast<float>(SKY_WIDTH) - 0.5f) * 2.0f * pi;
                        const glm::vec3 sd{std::sin(t) * std::cos(p), -std::sin(t) * std::sin(p), std::cos(t)};
                        inside += (glm::dot(sd, sun) >= cos_sun_radius) ? 1 : 0;
                    }
                }
                pixels[static_cast<size_t>(y) * SKY_WIDTH + x] += sun_color * (static_cast<float>(inside) / (SAMPLES * SAMPLES));
            }
        }
    }

    if (!writeHdr(path, SKY_WIDTH, SKY_HEIGHT, pixels)) {
        return false;
    }
    print("Saved the sky to {} ({}x{}). The sun is {} degrees up and gives {} times the sky's light", path.string(), SKY_WIDTH, SKY_HEIGHT,
          SUN_ELEVATION_DEGREES, SUN_TO_SKY);
    return true;
}

int main(int argc, char* argv[])
{
    if (argc != 3) {
        print("usage: mars_fissure_contentgen OUT.gcpak SKY.hdr");
        return EXIT_FAILURE;
    }
    const std::filesystem::path gcpak_path = argv[1];
    const std::filesystem::path sky_path = argv[2];

    std::vector<gcpak::GcpakCreator::Asset> assets{};
    makeGround(assets);

    gcpak::GcpakCreator creator{};
    for (const auto& asset : assets) {
        creator.addAsset(asset);
    }
    if (!creator.saveFile(gcpak_path)) {
        print("Failed to save {}", gcpak_path.string());
        return EXIT_FAILURE;
    }
    print("Saved {} assets to {}", assets.size(), gcpak_path.string());

    if (!makeSky(sky_path)) {
        print("Failed to save {}", sky_path.string());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
