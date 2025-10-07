#pragma once

#include <vector>

#include <gamecore/gc_render_mesh.h>

namespace gc {

std::vector<int> genTangents(std::vector<gc::MeshVertex>& vertices);

}