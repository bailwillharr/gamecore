#pragma once

#include "gamecore/gc_ecs.h"
#include "gamecore/gc_name.h"

namespace gc {

class World; // forward-dec

// Draws the widgets (ImGui) that show, and can edit, one component of an entity. The entity has the component when it is called.
using ComponentViewer = void (*)(World& world, Entity entity);

// The "World" debug window shows the values of components that have a viewer. The engine's own components have one already.
// A game can add viewers for its components, or replace one, at any time:
//     gc::registerComponentViewer(SpinComponent::NAME, [](gc::World& world, gc::Entity entity) {
//         ImGui::DragFloat("Speed", &world.getComponent<SpinComponent>(entity)->speed);
//     });
// A component without a viewer shows its serialised bytes, if it is serialisable.
void registerComponentViewer(Name component_name, ComponentViewer viewer);

// The "World" debug window: every entity in the world as a tree, and the components of the selected one.
// Call every frame. The window is only drawn if open isn't null (see DebugUI::getWindowOpen()).
void renderWorldUI(World& world, bool* open);

} // namespace gc
