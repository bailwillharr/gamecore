#pragma once

#include <cstdint>

#include <unordered_map>

#include "gamecore/gc_ecs.h"
#include "gamecore/gc_name.h"

namespace Rml {
class ElementDocument; // forward-dec
}

namespace gc {

class UI; // forward-dec

// Shows the documents of UIComponents (see gc_ui_component.h): loads a document when an entity gets a UIComponent, shows or hides it
// as the component says, and closes it when the component or the entity goes. Register it after the systems that create the data
// models that the documents use (see gc_ui.h), so that the models exist when the documents are first loaded.
// Only for games with a window: register it when the App has a UI.
class UISystem : public System {
public:
    static constexpr auto NAME = Name::createConstexpr("UISystem");

private:
    struct Document {
        Name name;
        Rml::ElementDocument* document; // null if it failed to load
        uint32_t reload_count;          // UI::getReloadCount() when it was loaded
        bool visible;
        bool seen; // in this update
    };

    UI& m_ui;
    std::unordered_map<Entity, Document> m_documents{};

public:
    UISystem(World& world, UI& ui);
    UISystem(const UISystem&) = delete;

    ~UISystem() override;

    UISystem& operator=(const UISystem&) = delete;

    void onUpdate(FrameState& frame_state) override;

    // The document that the entity's UIComponent shows, once the UISystem has updated since the component was added.
    // Null if there isn't one, or it failed to load.
    Rml::ElementDocument* getDocument(Entity entity) const;
};

} // namespace gc
