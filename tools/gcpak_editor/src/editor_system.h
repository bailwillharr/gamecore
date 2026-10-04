#pragma once

#include <array>
#include <string>
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

#include "asset_compiler.h"

namespace gc {
class Window; // forward-dec
}

class EditorSystem : public gc::System {
public:
    static constexpr auto NAME = gc::Name::createConstexpr("EditorSystem");

private:
    struct PakFileInfo {
        std::filesystem::path path;
    };

    struct EditorAsset {
        gcpak::GcpakCreator::Asset asset;
        PakFileInfo* from_file;
    };

    struct AssetCategoryList {
        std::vector<EditorAsset> assets;
    };

    gc::Window& m_window;
    gc::ResourceManager& m_resource_manager;

    std::mutex m_open_files_mutex{};
    std::vector<PakFileInfo> m_open_files{};
    std::atomic<bool> m_rescan = true;

    const SDL_DialogFileFilter m_gcpak_filter{.name = "Gamecore Package File (*.gcpak)", .pattern = "gcpak"};

    const std::array<SDL_DialogFileFilter, 5> m_asset_filters{
        SDL_DialogFileFilter{.name = "All Source Files", .pattern = "obj;png;jpg;jpeg;vert;frag;comp;gltf;glb"},
        SDL_DialogFileFilter{.name = "glTF Scene, becomes a prefab (*.gltf;*.glb)", .pattern = "gltf;glb"},
        SDL_DialogFileFilter{.name = "Mesh File (*.obj)", .pattern = "obj"},
        SDL_DialogFileFilter{.name = "Texture File (*.png;*.jpg)", .pattern = "png;jpg;jpeg"},
        SDL_DialogFileFilter{.name = "Shader File (*.vert;*.frag;*.comp)", .pattern = "vert;frag;comp"}};

    // What assets that were compiled in the editor, and so aren't in any package file yet, have as their from_file
    PakFileInfo m_unsaved_file{};

    // Source files and directories that were chosen in a dialog (on another thread) or dropped on the window, waiting to be compiled
    std::mutex m_pending_sources_mutex{};
    std::vector<std::filesystem::path> m_pending_sources{};

    std::string m_status{};

    std::mutex m_assets_mutex{};
    std::unordered_map<gcpak::GcpakAssetType, AssetCategoryList> m_assets{};

    std::optional<decltype(AssetCategoryList::assets)::const_iterator> m_selected_asset_it{};
    const EditorAsset* m_asset_being_previewed = nullptr;

    gc::Entity m_preview_entity = gc::ENTITY_NONE;
    gc::TransformComponent* m_preview_transform = nullptr;
    gc::RenderableComponent* m_preview_renderable = nullptr;
    gc::Name m_preview_mesh{};

    // A prefab being previewed is loaded into the world under this entity
    gc::Entity m_prefab_pivot = gc::ENTITY_NONE;
    // The resources that were made from assets so that the prefab can be drawn, to be deleted again afterwards
    std::vector<gc::Name> m_prefab_meshes{};
    std::vector<gc::Name> m_prefab_materials{};
    std::vector<gc::Name> m_prefab_textures{};

public:
    EditorSystem(gc::World& world, gc::Window& window, gc::ResourceManager& resource_manager, const std::filesystem::path& open_file);

    void onUpdate(gc::FrameState& frame_state) override;

private:
    static void SDLCALL openGcpakFileDialogCallback(void* userdata, const char* const* filelist, int filter);
    static void SDLCALL openAssetFileDialogCallback(void* userdata, const char* const* filelist, int filter);
    static void SDLCALL saveGcpakFileDialogCallback(void* userdata, const char* const* filelist, int filter);

    // Compiles the source files and directories that are waiting, and adds the resulting assets
    void processPendingSources();

    // returns nullptr if there is no such asset
    const Asset* findAsset(gcpak::GcpakAssetType type, uint32_t id);
    // of either texture type
    const Asset* findTextureAsset(uint32_t id);

    void showPrefabPreview(const Asset& prefab);
    void addPrefabPreviewMaterial(gc::Name material);
    void clearPrefabPreview();

    void showSelectedAssetInfoUI();

    void resetPreviewEntity();
};
