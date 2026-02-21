#pragma once

#include <unordered_map>
#include <vector>
#include <filesystem>
#include <mutex>
#include <vector>
#include <optional>
#include <filesystem>
#include <atomic>

#include <SDL3/SDL_dialog.h>

#include <gcpak/gcpak.h>

#include <gamecore/gc_ecs.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_transform_component.h>
#include <gamecore/gc_resource_manager.h>

namespace gc {
class Window; // forward-dec
}

class EditorSystem : public gc::System {

    struct PakFileInfo {
        std::filesystem::path path;
    };

    using EditorAsset = gcpak::GcpakCreator::Asset;

    struct AssetCategoryList {
        std::vector<EditorAsset> assets;
    };

    gc::Window& m_window;
    gc::ResourceManager& m_resource_manager;

    std::mutex m_open_files_mutex{};
    std::vector<PakFileInfo> m_open_files{};
    std::atomic<bool> m_rescan = true;

    const SDL_DialogFileFilter m_dialog_filter{.name = "Gamecore Package File (*.gcpak)", .pattern = "gcpak"};

    std::mutex m_assets_mutex{};
    std::unordered_map<gcpak::GcpakAssetType, AssetCategoryList> m_assets{};

    std::optional<decltype(AssetCategoryList::assets)::const_iterator> m_selected_asset_it{};
    const EditorAsset* m_asset_being_previewed = nullptr;

    gc::Entity m_preview_entity = gc::ENTITY_NONE;
    gc::TransformComponent* m_preview_transform = nullptr;
    gc::RenderableComponent* m_preview_renderable = nullptr;
    gc::Name m_preview_mesh{};

public:
    EditorSystem(gc::World& world, gc::Window& window, gc::ResourceManager& resource_manager, const std::filesystem::path& open_file);

    void onUpdate(gc::FrameState& frame_state) override;

private:
    static void SDLCALL openFileDialogCallback(void* userdata, const char* const* filelist, int filter);

    static void SDLCALL saveFileDialogCallback(void* userdata, const char* const* filelist, int filter);

    void showSelectedAssetInfoUI();

    void resetPreviewRenderable();
};
