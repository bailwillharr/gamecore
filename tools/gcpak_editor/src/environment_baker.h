#pragma once

// Turns an HDR panorama of a sky (a .hdr file: equirectangular, 2:1, with +Y up) into what the engine draws and lights a world
// with (see gc::SkyboxComponent). Three cube map assets are made, all in the engine's Z-up space:
//
//   "sky.hdr"              the sky itself, as it is seen. Sharp, and as bright as the file says (the sun included).
//   "sky.hdr/irradiance"   the light that a surface facing each direction gets from the whole sky: what diffuse surfaces
//                          reflect. Very blurry, so it is very small.
//   "sky.hdr/specular"     the sky as it is reflected by a surface of each roughness: sharp in the first mip level, for a
//                          mirror, and blurrier in each level after it.
//
// The last two are what "image based lighting" needs, worked out here so that the engine only has to sample them.
//
// The sun is left out of those two. A sky's panorama has the sun in it, thousands of times brighter than the rest, but the
// engine lights a world with the sun as a directional light (which has shadows, and baked ones at that). If the sun were also
// in the sky's light, everything would be lit by it twice, and once without shadows. So texels that are far brighter than the
// rest of the sky are dimmed to match it before the light is worked out. Where the sun was, and how bright, is stored in the
// first asset, so that a game can put its directional light in the same place.
//
// It is all done on the CPU, like baking shadows, so that packaging doesn't need a renderer.

#include <filesystem>
#include <vector>

#include "asset_compiler.h"

// Appends the three assets to assets_out. Returns false, having added nothing, if the file couldn't be read.
bool compileEnvironment(const std::filesystem::path& path, std::vector<Asset>& assets_out);
