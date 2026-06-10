#pragma once

#include <memory>
#include <ostream>
#include <istream>

#include "gamecore/gc_name.h"

namespace gc {

struct RenderableComponent {

public:
    static constexpr auto NAME = Name::createConstexpr("RenderableComponent");

public:
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

    void serialize(std::ostream& s) const
    {
        s.write(reinterpret_cast<const char*>(&m_visible), sizeof(bool));
        m_mesh.serialize(s);
        m_material.serialize(s);
    }

    static RenderableComponent deserialize(std::istream& s)
    {
        RenderableComponent r{};
        s.read(reinterpret_cast<char*>(&r.m_visible), sizeof(bool));
        r.m_mesh = Name::deserialize(s);
        r.m_material = Name::deserialize(s);
        return r;
    }
};

} // namespace gc
