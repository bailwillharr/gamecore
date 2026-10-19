#version 450

// Draws a terrain (see gc_terrain_system.h and gc_render_terrain.h). Each instance is a patch: a quarter of one of the terrain's
// tiles, as a grid of patch_quads x patch_quads quads. There is no vertex buffer: where a vertex is in the grid comes from its
// index, and its height from the tile, in the terrain's tile cache. Around the grid is a ring of vertices that are put in the same
// places as its edge, but lower down: the patch's skirts, which hide the gaps between patches of different levels of detail.
//
// As a patch gets further from the camera it turns into the patch of the level above (its parent) that covers the same ground:
// each vertex slides to the height that the parent's surface has there, and its normal to the parent's. By the time the patch is
// replaced by its parent, they look the same, so nothing pops. Neighbouring patches of different levels meet where the finer
// one has turned completely.

layout(push_constant) uniform PushConstants {
    mat4 world_transform;
    layout(offset = 112) float height_offset; // a sample's height is height_offset + its 16 bit value * height_scale
    float height_scale;
    uint tile_side;   // samples along a tile's side
    uint patch_quads; // quads along a patch's side
} pc;

layout(set = 0, binding = 0) uniform FrameUniformBuffer {
    mat4 projection;
    mat4 view;
    vec3 camera_position;
} frame_uniform_buffer;

// Every tile in the cache. A sample is the height in the low 16 bits, then the x and y of the normal (signed, of 127)
layout(set = 4, binding = 0) readonly buffer Tiles {
    uint samples[];
} tiles;

// The patch (see TerrainPatch)
layout(location = 0) in uvec4 in_patch_tiles; // the tile's offset, the parent tile's offset, where the patch starts in its tile
                                              // (and its level), where the tile starts in its parent
layout(location = 1) in vec4 in_patch_place;  // x, y (in the terrain's space), the spacing of its samples, the depth of its skirts
layout(location = 2) in vec2 in_patch_morph;  // the distances at which it starts turning into its parent, and has turned

layout(location = 0) out Vertex {
    vec3 position;         // in world space
    vec3 normal;           // in world space
    vec2 terrain_position; // x and y in the terrain's space, for tiling textures across it
} vout;
layout(location = 3) flat out uint out_level; // the level of detail, and bit 4 set to draw it in the level's colour

float decodeHeight(uint s) { return pc.height_offset + float(s & 0xffffu) * pc.height_scale; }

vec2 decodeNormal(uint s) { return vec2(float(int(s << 8) >> 24), float(int(s) >> 24)) / 127.0; }

uint readSample(uint tile, ivec2 position) { return tiles.samples[tile + uint(position.y) * pc.tile_side + uint(position.x)]; }

void main() {
    const int quads = int(pc.patch_quads);
    const uint side = pc.patch_quads + 3u;
    const ivec2 vertex = ivec2(int(uint(gl_VertexIndex) % side), int(uint(gl_VertexIndex) / side)) - 1;
    const bool skirt = any(lessThan(vertex, ivec2(0))) || any(greaterThan(vertex, ivec2(quads)));
    const ivec2 grid = clamp(vertex, ivec2(0), ivec2(quads));

    const uint origin = in_patch_tiles.z;
    const ivec2 s = ivec2(origin & 0xfffu, (origin >> 12) & 0xfffu) + grid; // the sample, in the tile
    out_level = (origin >> 24) & 0x1fu;

    const uint here = readSample(in_patch_tiles.x, s);
    float height = decodeHeight(here);
    vec2 normal_xy = decodeNormal(here);

    const vec2 terrain_position = in_patch_place.xy + vec2(grid) * in_patch_place.z;

    // How far the vertex has turned into the parent. (Measured before it moves, so that patches that share the vertex agree)
    const vec3 unmoved = vec3(pc.world_transform * vec4(terrain_position, height, 1.0));
    const float distance = length(unmoved - frame_uniform_buffer.camera_position);
    const float morph = clamp((distance - in_patch_morph.x) / max(in_patch_morph.y - in_patch_morph.x, 1.0e-3), 0.0, 1.0);
    if (morph > 0.0) {
        // Every other sample of this tile is a sample of the parent's. The ones in between are on the parent's edges and diagonals
        // (its cells are split from (0, 0) to (1, 1)), so they go halfway between two of the parent's samples.
        const ivec2 parent_origin = ivec2(in_patch_tiles.w & 0xffffu, in_patch_tiles.w >> 16);
        const ivec2 p = parent_origin + (s >> 1);
        const ivec2 odd = s & 1;
        const uint a = readSample(in_patch_tiles.y, p);
        float target_height = decodeHeight(a);
        vec2 target_normal = decodeNormal(a);
        if (odd != ivec2(0)) {
            const uint b = readSample(in_patch_tiles.y, p + odd);
            target_height = 0.5 * (target_height + decodeHeight(b));
            target_normal = 0.5 * (target_normal + decodeNormal(b));
        }
        height = mix(height, target_height, morph);
        normal_xy = mix(normal_xy, target_normal, morph);
    }
    if (skirt) {
        height -= in_patch_place.w;
    }

    const vec3 normal = vec3(normal_xy, sqrt(max(1.0 - dot(normal_xy, normal_xy), 0.0)));
    const vec4 world_position = pc.world_transform * vec4(terrain_position, height, 1.0);

    vout.position = vec3(world_position);
    vout.normal = normalize(mat3(pc.world_transform) * normal);
    vout.terrain_position = terrain_position;

    gl_Position = frame_uniform_buffer.projection * frame_uniform_buffer.view * world_position;
}
