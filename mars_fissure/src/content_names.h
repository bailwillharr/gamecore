#pragma once

// The names of the assets that the game loads, and that mars_fissure_contentgen and gcpak_editor make (see README)

// in content/mars_terrain.gcpak, baked from the DTM by gcpak_editor --bake-terrain
inline constexpr const char* TERRAIN_ASSET = "mars_fissure.terrain";

// in content/mars.gcpak, made by mars_fissure_contentgen
inline constexpr const char* GROUND_ALBEDO = "mars/ground_albedo";
inline constexpr const char* GROUND_NORMAL = "mars/ground_normal";
inline constexpr const char* GROUND_MATERIAL = "mars/ground";
// the sky's panorama is made by mars_fissure_contentgen, then baked into cube maps by gcpak_editor, which names them after the file
inline constexpr const char* SKY_ASSET = "mars_sky.hdr";
