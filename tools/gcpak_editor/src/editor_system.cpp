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

enum class ImGuiAnchorCorner { TopLeft, TopCenter, TopRight, CenterLeft, Center, CenterRight, BottomLeft, BottomCenter, BottomRight };

static void SetNextWindowPosAnchor(ImGuiCond cond, ImGuiAnchorCorner anchor, ImVec2 offset)
{
    ImGuiViewport* vp = ImGui::GetMainViewport();

    ImVec2 work_pos = vp->WorkPos;
    ImVec2 work_size = vp->WorkSize;

    ImVec2 pos;
    ImVec2 pivot;

    switch (anchor) {
        case ImGuiAnchorCorner::TopLeft:
            pos = ImVec2(work_pos.x, work_pos.y);
            pivot = ImVec2(0, 0);
            break;

        case ImGuiAnchorCorner::TopCenter:
            pos = ImVec2(work_pos.x + work_size.x * 0.5f, work_pos.y);
            pivot = ImVec2(0.5f, 0);
            break;

        case ImGuiAnchorCorner::TopRight:
            pos = ImVec2(work_pos.x + work_size.x, work_pos.y);
            pivot = ImVec2(1, 0);
            break;

        case ImGuiAnchorCorner::CenterLeft:
            pos = ImVec2(work_pos.x, work_pos.y + work_size.y * 0.5f);
            pivot = ImVec2(0, 0.5f);
            break;

        case ImGuiAnchorCorner::Center:
            pos = ImVec2(work_pos.x + work_size.x * 0.5f, work_pos.y + work_size.y * 0.5f);
            pivot = ImVec2(0.5f, 0.5f);
            break;

        case ImGuiAnchorCorner::CenterRight:
            pos = ImVec2(work_pos.x + work_size.x, work_pos.y + work_size.y * 0.5f);
            pivot = ImVec2(1, 0.5f);
            break;

        case ImGuiAnchorCorner::BottomLeft:
            pos = ImVec2(work_pos.x, work_pos.y + work_size.y);
            pivot = ImVec2(0, 1);
            break;

        case ImGuiAnchorCorner::BottomCenter:
            pos = ImVec2(work_pos.x + work_size.x, work_pos.y + work_size.y);
            pivot = ImVec2(0.5f, 1);
            pos.x = work_pos.x + work_size.x * 0.5f;
            break;

        case ImGuiAnchorCorner::BottomRight:
            pos = ImVec2(work_pos.x + work_size.x, work_pos.y + work_size.y);
            pivot = ImVec2(1, 1);
            break;
    }

    pos.x += offset.x;
    pos.y += offset.y;

    ImGui::SetNextWindowPos(pos, cond, pivot);
}

struct AABB {
    glm::vec3 min;
    glm::vec3 max;
};

static AABB getAABBFromMesh(const ResourceMesh& mesh)
{
    AABB aabb{};
    aabb.min.x = std::numeric_limits<float>::max();
    aabb.min.y = aabb.min.x;
    aabb.min.z = aabb.min.x;
    aabb.max.x = std::numeric_limits<float>::min();
    aabb.max.y = aabb.max.x;
    aabb.max.z = aabb.max.x;
    for (const auto& vertex : mesh.getVertices()) {
        aabb.min.x = glm::min(aabb.min.x, vertex.position.x);
        aabb.min.y = glm::min(aabb.min.y, vertex.position.y);
        aabb.min.z = glm::min(aabb.min.z, vertex.position.z);
        aabb.max.x = glm::max(aabb.max.x, vertex.position.x);
        aabb.max.y = glm::max(aabb.max.y, vertex.position.y);
        aabb.max.z = glm::max(aabb.max.z, vertex.position.z);
    }
    return aabb;
}

static void FitAABBToUnitCube(const AABB& box, glm::vec3& out_position, float& out_scale)
{
    // Compute size
    glm::vec3 size;
    size.x = box.max.x - box.min.x;
    size.y = box.max.y - box.min.y;
    size.z = box.max.z - box.min.z;

    // Compute center
    glm::vec3 center;
    center.x = (box.min.x + box.max.x) * 0.5f;
    center.y = (box.min.y + box.max.y) * 0.5f;
    center.z = (box.min.z + box.max.z) * 0.5f;

    // Find largest dimension
    float maxDim = size.x;
    if (size.y > maxDim) maxDim = size.y;
    if (size.z > maxDim) maxDim = size.z;

    // Uniform scale so largest dimension becomes 2
    out_scale = 2.0f / maxDim;

    // Position to move center to origin AFTER scaling
    out_position = glm::vec3{-center.x, -center.y, -center.z} * out_scale;
}

EditorSystem::EditorSystem(World& world, Window& window, gc::ResourceManager& resource_manager, const std::filesystem::path& open_file)
    : System(world), m_window(window), m_resource_manager(resource_manager)
{
    if (!open_file.empty()) {
        m_open_files.emplace_back(open_file);
    }
}

void EditorSystem::onUpdate(FrameState& frame_state)
{
    ZoneScoped;

    if (frame_state.window_state->getIsMouseCaptured()) {
        // when engine closes debug UI, it tries to recapture the mouse
        m_window.setMouseCaptured(false);
    }

    if (const auto& drag_drop_path = frame_state.window_state->getDragDropPath(); !drag_drop_path.empty()) {
        // terrible hack right here
        // callback needs a null-terminated ARRAY of null-terminated strings
        const std::array<const char*, 2> filelist{drag_drop_path.c_str(), nullptr};
        openGcpakFileDialogCallback(this, filelist.data(), 0);
    }

    SetNextWindowPosAnchor(ImGuiCond_Always, ImGuiAnchorCorner::BottomRight, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("Files", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing)) {
        if (ImGui::Button("Rescan Files")) {
            m_rescan.store(true, std::memory_order_relaxed);
        }
        if (ImGui::Button("Open New File")) {
            SDL_ShowOpenFileDialog(openGcpakFileDialogCallback, this, m_window.getHandle(), &m_gcpak_filter, 1, NULL, true);
        }
        if (ImGui::Button("Save All To Package File")) {
            SDL_ShowSaveFileDialog(saveGcpakFileDialogCallback, this, m_window.getHandle(), &m_gcpak_filter, 1,
                                   App::instance().getSaveDirectory().string().c_str());
        }
        if (ImGui::Button("Add Asset")) {
            SDL_ShowOpenFileDialog(openAssetFileDialogCallback, this, m_window.getHandle(), m_asset_filters.data(), static_cast<int>(m_asset_filters.size()),
                                   NULL, true);
        }
    }
    ImGui::End();

    {
        std::unique_lock assets_lock(m_assets_mutex);

        // reload files from disk
        if (m_rescan.exchange(false, std::memory_order_relaxed)) {

            m_selected_asset_it = {};

            gcpak::GcpakCreator creator{};

            {
                std::unique_lock open_files_lock(m_open_files_mutex);

                // only erase assets that are associated with an open gcpak file.
                // My guess is this will be slow the first time reloading after an asset is manually added.
                // But for times after that, assets from .gcpak files will be at the end of the list (so no reallocations).
                for (auto& [type, category_list] : m_assets) {
                    auto& list = category_list.assets;
                    for (auto it = list.begin(); it != list.end();) {
                        if (!it->from_file->path.empty()) {
                            it = list.erase(it);
                        }
                        else {
                            ++it;
                        }
                    }
                }

                for (auto it = m_open_files.begin(); it != m_open_files.end();) {
                    creator.clear();
                    auto& file = *it;
                    if (std::error_code ec; !creator.loadFile(file.path, ec)) {
                        GC_ERROR("error loading gcpak file or hash file: {}, error: {}", file.path.string(), ec.message());
                        it = m_open_files.erase(it);
                    }
                    else {
                        for (const auto& asset : creator.getAssets()) {
                            if (asset.hash != crc32(asset.name)) {
                                gc::abortGame("Invalid hash for asset: {} Actual: {:#08x}, Saved: {:#08x}", asset.name, crc32(asset.name), asset.hash);
                            }

                            EditorAsset editor_asset{};
                            editor_asset.asset.name = asset.name;
                            editor_asset.asset.hash = asset.hash;
                            editor_asset.asset.type = asset.type;
                            editor_asset.asset.data = asset.data;
                            editor_asset.from_file = &file;
                            m_assets[asset.type].assets.push_back(std::move(editor_asset));
                        }
                        ++it;
                    }
                }
            }
        }

        if (!m_assets.empty()) {
            SetNextWindowPosAnchor(ImGuiCond_Always, ImGuiAnchorCorner::TopLeft, ImVec2(0.0f, 0.0f));
            if (ImGui::Begin("Asset List", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing)) {
                for (auto& [type, category_list] : m_assets) {
                    auto type_string = getAssetTypeString(type);
                    ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
                    if (ImGui::CollapsingHeader(type_string.c_str(), nullptr)) {
                        for (auto it = category_list.assets.begin(); it != category_list.assets.end(); ++it) {

                            bool selected = false;
                            if (m_selected_asset_it) {
                                if (m_selected_asset_it.value()->asset.type == it->asset.type) {
                                    // need to do this as MSVC throws assert failure for 'iterator types not compatible'
                                    if (m_selected_asset_it.value() == it) {
                                        selected = true;
                                    }
                                }
                            }

                            std::string asset_name = it->asset.name;
                            if (asset_name.empty()) {
                                asset_name = std::to_string(it->asset.hash);
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
        }

        if (!m_open_files.empty()) {
            SetNextWindowPosAnchor(ImGuiCond_Always, ImGuiAnchorCorner::BottomLeft, ImVec2(0.0f, 0.0f));
            if (ImGui::Begin(
                    "Open Files", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing)) {
                for (const auto& open_file : m_open_files) {
                    ImGui::Text("%s", open_file.path.filename().string().c_str());
                }
            }
            ImGui::End();
        }

        showSelectedAssetInfoUI();

        if (m_preview_entity == gc::ENTITY_NONE) {
            m_preview_entity = m_world.createEntity("preview_entity"_name);
            m_world.addComponent<RenderableComponent>(m_preview_entity);
            m_preview_transform = m_world.getComponent<TransformComponent>(m_preview_entity);
            m_preview_renderable = m_world.getComponent<RenderableComponent>(m_preview_entity);
            resetPreviewEntity();
        }

        if (m_preview_mesh.empty()) {
            m_preview_mesh = m_resource_manager.add<ResourceMesh>(genCubeMesh());
        }

        if (m_selected_asset_it) {
            const auto& asset = *m_selected_asset_it.value();
            if (m_asset_being_previewed != &asset) {

                resetPreviewEntity();

                switch (asset.asset.type) {
                    case gcpak::GcpakAssetType::TEXTURE_R8G8B8A8: {

                        ResourceTexture new_texture{};
                        new_texture.data = asset.asset.data;
                        new_texture.srgb = true;
                        const gc::Name new_texture_name = m_resource_manager.add<ResourceTexture>(std::move(new_texture));

                        ResourceMaterial new_material{};
                        new_material.base_color_texture = new_texture_name;
                        const gc::Name new_material_name = m_resource_manager.add<ResourceMaterial>(std::move(new_material));

                        m_preview_renderable->setMesh(m_preview_mesh);
                        m_preview_renderable->setMaterial(new_material_name);
                        m_preview_renderable->setVisible(true);

                        const auto texture_info = getAssetTextureInfo(asset.asset.data);

                        const float scale_xy = static_cast<float>(texture_info.width) / static_cast<float>(texture_info.height);
                        m_preview_transform->setScale(scale_xy, scale_xy, 1.0f);
                    } break;
                    case gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16: {
                        ResourceMesh new_mesh = createMeshFromData(asset.asset.data);
                        const AABB aabb = getAABBFromMesh(new_mesh);
                        const gc::Name new_mesh_name = m_resource_manager.add<ResourceMesh>(std::move(new_mesh));

                        m_preview_renderable->setMesh(new_mesh_name);
                        m_preview_renderable->setMaterial({});
                        m_preview_renderable->setVisible(true);

                        glm::vec3 position{};
                        float scale{};
                        FitAABBToUnitCube(aabb, position, scale);
                        position += glm::vec3{0.0f, 5.0f, 0.0f};

                        m_preview_transform->setPosition(position);
                        m_preview_transform->setScale(scale);
                    } break;
                    default:
                        break;
                }
                m_asset_being_previewed = &asset;
            }
        }
        else {
            m_asset_being_previewed = nullptr;
            resetPreviewEntity();
        }
    }

    static float angle = 0.0f;
    angle += static_cast<float>(frame_state.delta_time);
    m_preview_transform->setRotation(glm::angleAxis(angle, glm::vec3(0.0f, 0.0f, 1.0f)));
}

// This can be called on a different thread. On Windows 10, it is.
void SDLCALL EditorSystem::openGcpakFileDialogCallback(void* userdata, const char* const* filelist, int)
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
            GC_ASSERT(*file_path_ptr);
            PakFileInfo file_info{};
            file_info.path = *file_path_ptr;
            bool already_exists = false;
            for (const auto& existing_opened_file : self->m_open_files) {
                if (existing_opened_file.path == file_info.path) {
                    already_exists = true;
                    GC_WARN("EditorSystem::openGcpakFileDialogCallback: file already opened: {}", file_info.path.string());
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

void SDLCALL EditorSystem::openAssetFileDialogCallback(void* userdata, const char* const* filelist, int)
{
    EditorSystem* const self = static_cast<EditorSystem*>(userdata);
    GC_ASSERT(self);

    if (!filelist) {
        GC_ERROR("SDL_DialogFileCallback error: {}", SDL_GetError());
        return;
    }

    {
        std::unique_lock lock(self->m_open_files_mutex);
    }

    self->m_rescan.store(true, std::memory_order_relaxed);
}

void SDLCALL EditorSystem::saveGcpakFileDialogCallback(void* userdata, const char* const* filelist, int filter)
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
                creator.addAsset(asset.asset);
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

    SetNextWindowPosAnchor(ImGuiCond_Always, ImGuiAnchorCorner::TopRight, ImVec2(0.0f, 0.0f));
    if (ImGui::Begin("Asset Info", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("Name: %s", asset.asset.name.c_str());
        ImGui::Text("Hash: %#x", asset.asset.hash);
        ImGui::Text("Data Size: %s", bytesToHumanReadable(asset.asset.data.size()).c_str());
        ImGui::Text("Type: %s", getAssetTypeString(asset.asset.type).c_str());
        ImGui::Text("From file: %s", asset.from_file->path.filename().string().c_str()); // FML

        switch (asset.asset.type) {
            case gcpak::GcpakAssetType::TEXTURE_R8G8B8A8: {
                auto info = getAssetTextureInfo(asset.asset.data);
                ImGui::Text("Width: %u, Height: %u", info.width, info.height);
            } break;
            case gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16: {
                auto info = getAssetMeshInfo(asset.asset.data);
                ImGui::Text("Vertices: %d, Triangles: %d", info.vertex_count, info.index_count / 3);
            } break;
        }

        if (ImGui::Button("Remove")) {
            auto& list = m_assets[asset.asset.type].assets;
            list.erase(asset_it);
            m_selected_asset_it = {}; // now invalidated
        }
    }
    ImGui::End();
}

void EditorSystem::resetPreviewEntity()
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

    m_preview_transform->setPosition(0.0f, 5.0f, 0.0f);
    m_preview_transform->setScale(1.0f);
}
