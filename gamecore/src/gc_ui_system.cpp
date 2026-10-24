#include "gamecore/gc_ui_system.h"

#include <RmlUi/Core/ElementDocument.h>

#include <tracy/Tracy.hpp>

#include "gamecore/gc_frame_state.h"
#include "gamecore/gc_ui.h"
#include "gamecore/gc_ui_component.h"
#include "gamecore/gc_world.h"
#include "gclog/gclog.h"

namespace gc {

UISystem::UISystem(World& world, UI& ui) : System(world), m_ui(ui) {}

UISystem::~UISystem()
{
    for (auto& [entity, document] : m_documents) {
        if (document.document) {
            document.document->Close();
        }
    }
}

void UISystem::onUpdate([[maybe_unused]] FrameState& frame_state)
{
    ZoneScoped;

    for (auto& [entity, document] : m_documents) {
        document.seen = false;
    }

    const uint32_t reload_count = m_ui.getReloadCount();
    m_world.forEach<UIComponent>([&](Entity entity, const UIComponent& component) {
        auto it = m_documents.find(entity);
        if (it != m_documents.end() && (it->second.name != component.document || it->second.reload_count != reload_count)) {
            // a different document (or the files have changed): close this one, and load the new one below
            if (it->second.document) {
                it->second.document->Close();
            }
            m_documents.erase(it);
            it = m_documents.end();
        }
        if (it == m_documents.end()) {
            Document document{};
            document.name = component.document;
            document.reload_count = reload_count;
            document.document = component.document.empty() ? nullptr : m_ui.loadDocument(component.document);
            if (!document.document && !component.document.empty()) {
                GC_ERROR("Entity {} has a UIComponent whose document {} failed to load", entity, component.document);
            }
            document.visible = false;
            it = m_documents.emplace(entity, document).first;
        }

        Document& document = it->second;
        document.seen = true;
        if (document.document && document.visible != component.visible) {
            if (component.visible) {
                document.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
            }
            else {
                document.document->Hide();
            }
            document.visible = component.visible;
        }
    });

    // the components that have gone
    for (auto it = m_documents.begin(); it != m_documents.end();) {
        if (!it->second.seen) {
            if (it->second.document) {
                it->second.document->Close();
            }
            it = m_documents.erase(it);
        }
        else {
            ++it;
        }
    }
}

Rml::ElementDocument* UISystem::getDocument(Entity entity) const
{
    const auto it = m_documents.find(entity);
    return (it != m_documents.end()) ? it->second.document : nullptr;
}

} // namespace gc
