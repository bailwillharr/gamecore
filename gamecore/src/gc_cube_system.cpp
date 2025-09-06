#include "gamecore/gc_cube_system.h"

#include <tracy/Tracy.hpp>

#include "gamecore/gc_cube_component.h"
#include "gamecore/gc_transform_component.h"
#include "gamecore/gc_world.h"

namespace gc {

CubeSystem::CubeSystem(gc::World& world) : gc::System(world, Signature::fromTypes<CubeComponent, TransformComponent>()) {}

void CubeSystem::onUpdate(double dt)
{
    ZoneScoped;

    (void)dt;

    m_cube_transforms.clear();

    m_world.forEach<TransformComponent, CubeComponent>([&]([[maybe_unused]] Entity entity, TransformComponent& t, CubeComponent& c) {
        if (c.visible) {
            m_cube_transforms.push_back(t.getWorldMatrix());
        }
    });
}

} // namespace gc