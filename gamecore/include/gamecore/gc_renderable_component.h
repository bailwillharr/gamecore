#pragma once

#include <memory>

#include "gamecore/gc_name.h"

namespace gc {

class RenderSystem; // forward-dec

struct RenderableComponent {
    bool m_visible = true;
    Name m_mesh{};     // can be empty
    Name m_material{}; // can be empty

public:
    RenderableComponent& setVisible(bool visible)
    {
        m_visible = visible;
        return *this;
    }

    RenderableComponent& setMesh(Name mesh)
    {
        m_mesh = mesh;
        return *this;
    }

    RenderableComponent& setMaterial(Name material)
    {
        m_material = material;
        return *this;
    }
};

} // namespace gc
