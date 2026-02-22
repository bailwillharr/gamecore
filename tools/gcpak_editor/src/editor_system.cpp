#include "editor_system.h"

#include <imgui.h>

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_messagebox.h>

#include <tracy/Tracy.hpp>

#include <gamecore/gc_app.h>
#include <gamecore/gc_frame_state.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_units.h>
#include <gamecore/gc_world.h>
#include <gamecore/gc_resource_manager.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_resources.h>
#include <gamecore/gc_gen_mesh.h>

using namespace gc;
using namespace gc::literals;

static std::string getAssetTypeString(gcpak::GcpakAssetType type)
{
    using gcpak::GcpakAssetType;
    switch (type) {
        case GcpakAssetType::INVALID:
            return "INVALID";
        case GcpakAssetType::SPIRV_SHADER:
            return "Shader";
        case GcpakAssetType::TEXTURE_R8G8B8A8:
            return "Texture";
        case GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16:
            return "Mesh";
        case GcpakAssetType::PREFAB:
            return "Prefab";
        default:
            return "(unknown)";
    }
}

struct AssetTextureInfo {
    uint32_t width;
    uint32_t height;
};

static AssetTextureInfo getAssetTextureInfo(const std::span<const uint8_t> data)
{
    AssetTextureInfo info{};

    auto src = data.data();

    const auto data_end = src + data.size();

    if (src + sizeof(uint32_t) < data_end) {
        std::memcpy(&info.width, src, sizeof(uint32_t));
    }

    src += sizeof(uint32_t);
    if (src + sizeof(uint32_t) < data_end) {
        std::memcpy(&info.height, src, sizeof(uint32_t));
    }

    return info;
}

struct AssetMeshInfo {
    int vertex_count;
    int index_count;
};

static AssetMeshInfo getAssetMeshInfo(const std::span<const uint8_t> data)
{
    GC_ASSERT(data.size() > sizeof(uint16_t));

    uint16_t vertex_count{};
    std::memcpy(&vertex_count, data.data(), sizeof(uint16_t));

    const uint8_t* const vertices_location = data.data() + sizeof(uint16_t);
    const uint8_t* const indices_location = vertices_location + vertex_count * sizeof(MeshVertex);
    const size_t index_count = (data.data() + data.size() - indices_location) / sizeof(uint16_t);

    AssetMeshInfo info{};
    info.vertex_count = static_cast<int>(vertex_count);
    info.index_count = static_cast<int>(index_count);

    return info;
}

static ResourceMesh createMeshFromData(const std::span<const uint8_t> data)
{
    GC_ASSERT(data.size() > sizeof(uint16_t));

    uint16_t vertex_count{};
    std::memcpy(&vertex_count, data.data(), sizeof(uint16_t));

    const uint8_t* const vertices_location = data.data() + sizeof(uint16_t);
    const uint8_t* const indices_location = vertices_location + vertex_count * sizeof(MeshVertex);
    const size_t index_count = (data.data() + data.size() - indices_location) / sizeof(uint16_t);

    GC_ASSERT(data.size() == sizeof(uint16_t) + vertex_count * sizeof(MeshVertex) + index_count * sizeof(uint16_t));

    const auto vertices_begin = reinterpret_cast<const MeshVertex*>(vertices_location);
    const auto vertices_end = vertices_begin + vertex_count;
    const auto indices_begin = reinterpret_cast<const uint16_t*>(indices_location);
    const auto indices_end = indices_begin + index_count;

    const std::vector<MeshVertex> vertices(vertices_begin, vertices_end);
    const std::vector<uint16_t> indices(indices_begin, indices_end);

    return ResourceMesh(vertices, indices);
}

EditorSystem::EditorSystem(World& world, Window& window, gc::ResourceManager& resource_manager, const std::filesystem::path& open_file)
    : System(world), m_window(window), m_resource_manager(resource_manager)
{
    m_open_files.emplace_back(open_file);
}

void EditorSystem::onUpdate(FrameState& frame_state)
{
    ZoneScoped;

    if (frame_state.window_state->getIsMouseCaptured()) {
        // when engine closes debug UI, it tries to recapture the mouse
        m_window.setMouseCaptured(false);
    }

    if (ImGui::Begin("Files")) {
        if (ImGui::Button("Rescan Files")) {
            m_rescan.store(true, std::memory_order_relaxed);
        }
        if (ImGui::Button("Open New File")) {
            SDL_ShowOpenFileDialog(openFileDialogCallback, this, m_window.getHandle(), &m_dialog_filter, 1, NULL, true);
        }
        if (ImGui::Button("Save As")) {
            SDL_ShowSaveFileDialog(saveFileDialogCallback, this, m_window.getHandle(), &m_dialog_filter, 1,
                                   App::instance().getSaveDirectory().string().c_str());
        }
        {
            std::array<char, 64> name_buf{};
            ImGui::Text("Add Asset:");
            ImGui::InputText("NAME", name_buf.data(), name_buf.size(), ImGuiInputTextFlags_CharsNoBlank);
            ImGui::Text("Hash: %#x", crc32(name_buf.data()));
        }
    }
    ImGui::End();

    {
        std::unique_lock assets_lock(m_assets_mutex);

        // exchange returns the original value, and replaces the value with the argument
        if (m_rescan.exchange(false, std::memory_order_relaxed)) {

            m_selected_asset_it = {};

            gcpak::GcpakCreator creator{};

            {
                std::unique_lock open_files_lock(m_open_files_mutex);
                m_assets.clear();
                for (auto& file : m_open_files) {
                    creator.clear();
                    if (std::error_code ec; !creator.loadFile(file.path, ec)) {
                        GC_ERROR("error loading gcpak file or hash file: {}, error: {}", file.path.string(), ec.message());
                    }
                    for (const auto& asset : creator.getAssets()) {
                        if (asset.hash != crc32(asset.name)) {
                            gc::abortGame("Invalid hash for asset: {} Actual: {:#08x}, Saved: {:#08x}", asset.name, crc32(asset.name), asset.hash);
                        }

                        EditorAsset editor_asset{};
                        editor_asset.name = asset.name;
                        editor_asset.hash = asset.hash;
                        editor_asset.type = asset.type;
                        editor_asset.data = asset.data;
                        m_assets[asset.type].assets.push_back(std::move(editor_asset));
                    }
                }
            }
        }

        if (ImGui::Begin("Asset List")) {
            for (auto& [type, category_list] : m_assets) {
                auto type_string = getAssetTypeString(type);
                ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
                if (ImGui::CollapsingHeader(type_string.c_str(), nullptr)) {
                    for (auto it = category_list.assets.begin(); it != category_list.assets.end(); ++it) {

                        bool selected = false;
                        if (m_selected_asset_it) {
                            if (m_selected_asset_it.value()->type == it->type) {
                                // need to do this as MSVC throws assert failure for 'iterator types not compatible'
                                if (m_selected_asset_it.value() == it) {
                                    selected = true;
                                }
                            }
                        }

                        std::string asset_name = it->name;
                        if (asset_name.empty()) {
                            asset_name = std::to_string(it->hash);
                        }
                        if (ImGui::Selectable(asset_name.c_str(), selected)) {
                            if (selected) {
                                m_selected_asset_it = {};
                            }
                            else {
                                m_selected_asset_it = it;
                            }
                        }
                    }
                }
            }
        }
        ImGui::End();

        if (ImGui::Begin("Open Files")) {
            for (const auto& open_file : m_open_files) {
                ImGui::Text("%s", open_file.path.filename().string().c_str());
            }
        }
        ImGui::End();

        showSelectedAssetInfoUI();

        if (m_preview_entity == gc::ENTITY_NONE) {
            m_preview_entity = m_world.createEntity("preview_entity"_name, gc::ENTITY_NONE, {0.0f, 5.0f, 0.0f});
            m_world.addComponent<RenderableComponent>(m_preview_entity);
        }
        if (!m_preview_transform) {
            m_preview_transform = m_world.getComponent<TransformComponent>(m_preview_entity);
        }
        if (!m_preview_renderable) {
            m_preview_renderable = m_world.getComponent<RenderableComponent>(m_preview_entity);
        }
        if (m_preview_mesh.empty()) {
            m_preview_mesh = m_resource_manager.add<ResourceMesh>(genCubeMesh());
        }

        if (m_selected_asset_it) {
            const auto& asset = *m_selected_asset_it.value();
            if (m_asset_being_previewed != &asset) {

                resetPreviewRenderable();

                switch (asset.type) {
                    case gcpak::GcpakAssetType::TEXTURE_R8G8B8A8: {

                        ResourceTexture new_texture{};
                        new_texture.data = asset.data;
                        const gc::Name new_texture_name = m_resource_manager.add<ResourceTexture>(std::move(new_texture));

                        ResourceMaterial new_material{};
                        new_material.base_color_texture = new_texture_name;
                        const gc::Name new_material_name = m_resource_manager.add<ResourceMaterial>(std::move(new_material));

                        m_preview_renderable->setMesh(m_preview_mesh);
                        m_preview_renderable->setMaterial(new_material_name);
                        m_preview_renderable->setVisible(true);
                    } break;
                    case gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16: {
                        ResourceMesh new_mesh = createMeshFromData(asset.data);
                        const gc::Name new_mesh_name = m_resource_manager.add<ResourceMesh>(std::move(new_mesh));

                        m_preview_renderable->setMesh(new_mesh_name);
                        m_preview_renderable->setMaterial({});
                        m_preview_renderable->setVisible(true);
                    } break;
                    default:
                        break;
                }
                m_asset_being_previewed = &asset;
            }
        }
        else {
            m_asset_being_previewed = nullptr;
            resetPreviewRenderable();
        }
    }
}

// This can be called on a different thread. On Windows 10, it is.
void SDLCALL EditorSystem::openFileDialogCallback(void* userdata, const char* const* filelist, [[maybe_unused]] int filter)
{
    EditorSystem* const self = static_cast<EditorSystem*>(userdata);
    GC_ASSERT(self);

    if (!filelist) {
        GC_ERROR("SDL_DialogFileCallback error: {}", SDL_GetError());
        return;
    }

    bool added_files = false;

    {
        std::unique_lock lock(self->m_open_files_mutex);
        for (const char* const* file_path_ptr = filelist; *file_path_ptr; ++file_path_ptr) {
            PakFileInfo file_info{};
            file_info.path = *file_path_ptr;
            bool already_exists = false;
            for (const auto& existing_opened_file : self->m_open_files) {
                if (existing_opened_file.path == file_info.path) {
                    already_exists = true;
                    GC_WARN("EditorSystem::openFileDialogCallback: file already opened: {}", file_info.path.string());
                    break;
                }
            }
            if (!already_exists) {
                self->m_open_files.push_back(std::move(file_info));
                added_files = true;
            }
        }
    }

    self->m_rescan.store(true, std::memory_order_relaxed);
}

void SDLCALL EditorSystem::saveFileDialogCallback(void* userdata, const char* const* filelist, [[maybe_unused]] int filter)
{
    EditorSystem* const self = static_cast<EditorSystem*>(userdata);
    GC_ASSERT(self);

    if (!filelist) {
        GC_ERROR("SDL_DialogFileCallback error: {}", SDL_GetError());
        return;
    }

    if (!filelist[0]) {
        GC_ERROR("No save file specified!");
        return;
    }

    std::filesystem::path save_path(filelist[0]);

    if (filter == 0) { // .gcpak extension filter selected
        save_path.replace_extension("gcpak");
    }

    gcpak::GcpakCreator creator{};

    {
        std::unique_lock lock(self->m_assets_mutex);

        for (const auto& [type, category] : self->m_assets) {
            for (const auto& asset : category.assets) {
                creator.addAsset(asset);
            }
        }
    }

    if (!creator.saveFile(save_path)) {
        GC_ERROR("Failed to save file: {}", save_path.string());
    }
}

void EditorSystem::showSelectedAssetInfoUI()
{
    if (!m_selected_asset_it) {
        return;
    }
    const auto& asset_it = m_selected_asset_it.value();
    const auto& asset = *asset_it;

    if (ImGui::Begin("Asset Info")) {
        ImGui::Text("Name: %s", asset.name.c_str());
        ImGui::Text("Hash: %#x", asset.hash);
        ImGui::Text("Data Size: %s", bytesToHumanReadable(asset.data.size()).c_str());

        ImGui::Text("Type: %s", getAssetTypeString(asset.type).c_str());

        switch (asset.type) {
            case gcpak::GcpakAssetType::TEXTURE_R8G8B8A8: {
                auto info = getAssetTextureInfo(asset.data);
                ImGui::Text("Width: %u, Height: %u", info.width, info.height);
            } break;
            case gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16: {
                auto info = getAssetMeshInfo(asset.data);
                ImGui::Text("Vertices: %d, Triangles: %d", info.vertex_count, info.index_count / 3);
            } break;
        }

        if (ImGui::Button("Remove")) {
            auto& list = m_assets[asset.type].assets;
            list.erase(asset_it);
            m_selected_asset_it = {}; // now invalidated
        }
    }
    ImGui::End();
}

void EditorSystem::resetPreviewRenderable()
{
    if (!m_preview_renderable->m_material.empty()) {
        const ResourceMaterial* material = m_resource_manager.get<ResourceMaterial>(m_preview_renderable->m_material);
        if (material) {
            if (!material->base_color_texture.empty()) {
                m_resource_manager.deleteResource<ResourceTexture>(material->base_color_texture);
            }
            GC_ASSERT(material->orm_texture.empty());
            GC_ASSERT(material->normal_texture.empty());
        }
        m_resource_manager.deleteResource<ResourceMaterial>(m_preview_renderable->m_material);
    }

    if (!m_preview_renderable->m_mesh.empty() && m_preview_renderable->m_mesh != m_preview_mesh) {
        m_resource_manager.deleteResource<ResourceMesh>(m_preview_renderable->m_mesh);
    }

    m_preview_renderable->setVisible(false);
    m_preview_renderable->setMaterial({});
    m_preview_renderable->setMesh({});
}