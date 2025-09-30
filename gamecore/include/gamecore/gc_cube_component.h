#pragma once

#include <memory>

namespace gc {

class RenderMesh;     // forward-dec
class RenderMaterial; // forward-dec
class CubeSystem; // forward-dec

class CubeComponent {
    friend class CubeSystem;

    bool m_visible = true;
    RenderMesh* m_mesh{};         // can be nullptr
    RenderMaterial* m_material{}; // can be nullptr

public:
    CubeComponent& setVisible(bool visible)
    {
        m_visible = visible;
        return *this;
    }

    CubeComponent& setMesh(RenderMesh* mesh)
    {
        m_mesh = mesh;
        return *this;
    }

    CubeComponent& setMaterial(RenderMaterial* material)
    {
        m_material = material;
        return *this;
    }
};

} // namespace gc
