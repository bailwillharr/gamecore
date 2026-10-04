#pragma once

#include <filesystem>
#include <vector>

#include "asset_compiler.h"

// Compiles the default scene of a glTF file (.gltf or .glb) into a prefab, named after the file (e.g. "robot.glb").
// The meshes, materials and textures that the scene uses become assets of their own, named "robot.glb/mesh0_0" and so on, which the
// prefab's RenderableComponents refer to. Appends all of them to assets_out. Returns false, having added nothing, on failure.
//
// What is and isn't converted:
//  - Nodes become entities with the same hierarchy, under a root entity that rotates glTF's Y-up coordinates to the engine's Z-up.
//  - A node's mesh becomes a RenderableComponent. If the mesh has several primitives, each one gets a child entity.
//    Only triangle lists are supported. Normals and tangents are generated if they are missing.
//    Meshes with too many vertices for one mesh asset are split up.
//  - Materials: base color (an sRGB texture), metallic-roughness, occlusion and normal textures (linear). Constant factors are baked into the textures, as
//    the engine's materials are only textures. Emission, transparency, double sided materials and texture transforms are ignored.
//  - Perspective cameras become (inactive) CameraComponents. KHR_lights_punctual lights become LightComponents.
//  - Skins, animations and morph targets are ignored.
bool compileGltf(const std::filesystem::path& path, std::vector<Asset>& assets_out);
