#pragma once

#include <memory>

namespace gc {

class RenderMesh;

class CubeComponent {
public:
    bool visible = true;
    RenderMesh* mesh{};
};

} // namespace gc
