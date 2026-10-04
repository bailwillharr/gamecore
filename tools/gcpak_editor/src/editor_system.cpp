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
#include <gamecore/gc_byte_reader.h>
#include <gamecore/gc_prefab.h>
#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_light_component.h>

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
        return "Texture (linear)";
    case GcpakAssetType::TEXTURE_R8G8B8A8_SRGB:
        return "Texture (sRGB)";
    case GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16:
        return "Mesh";
    case GcpakAssetType::PREFAB:
        return "Prefab";
    case GcpakAssetType::MATERIAL:
        return "Material";
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
    aabb.max.x = std::numeric_limits<float>::lowest();
    aabb.max.y = aabb.max.x;
    aabb.max.z = aabb.max.x;
    for (const auto& vertex : mesh.vertices.get()) {
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
    glm::vec3 size{};
    size.x = box.max.x - box.min.x;
    size.y = box.max.y - box.min.y;
    size.z = box.max.z - box.min.z;

    // Compute center
    glm::vec3 center{};
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

static glm::mat4 getLocalMatrix(const TransformComponent& t)
{
    glm::mat4 matrix = glm::mat4_cast(t.getRotation());
    matrix[3] = glm::vec4(t.getPosition(), 1.0f);
    return glm::scale(matrix, t.getScale());
}

// The transform of an entity relative to one of its ancestors. (World matrices are only up to date after the TransformSystem has run.)
static glm::mat4 getMatrixRelativeTo(World& world, Entity entity, Entity ancestor)
{
    glm::mat4 matrix{1.0f};
    while (entity != ENTITY_NONE && entity != ancestor) {
        const TransformComponent* const t = world.getComponent<TransformComponent>(entity);
        matrix = getLocalMatrix(*t) * matrix;
        entity = t->getParent();
    }
    return matrix;
}

struct AssetPrefabInfo {
    uint32_t entity_count{};
    std::vector<std::pair<gc::Name, uint32_t>> component_counts{}; // other than transforms
    bool valid{true};
};

static AssetPrefabInfo getAssetPrefabInfo(const std::span<const uint8_t> data)
{
    AssetPrefabInfo info{};
    ByteReader reader(data);
    while (reader.remaining() > 0) {
        if (reader.remaining() < gcpak::PREFAB_COMPONENT_HEADER_SIZE) {
            info.valid = false;
            break;
        }
        const gc::Name component_name(reader.readU32());
        const size_t size = reader.readU32();
        if (size > reader.remaining()) {
            info.valid = false;
            break;
        }
        reader.skip(size);
        if (component_name == TransformComponent::NAME) {
            ++info.entity_count;
            continue;
        }
        auto it = std::find_if(info.component_counts.begin(), info.component_counts.end(), [&](const auto& pair) { return pair.first == component_name; });
        if (it == info.component_counts.end()) {
            info.component_counts.emplace_back(component_name, 1);
        }
        else {
            ++it->second;
        }
    }
    return info;
}

static std::string getComponentDisplayName(gc::Name component_name)
{
    if (component_name == RenderableComponent::NAME) {
        return "RenderableComponent";
    }
    else if (component_name == CameraComponent::NAME) {
        return "CameraComponent";
    }
    else if (component_name == LightComponent::NAME) {
        return "LightComponent";
    }
    else {
        return component_name.getString(); // only readable if the name happens to be known
    }
}

EditorSystem::EditorSystem(World& world, Window& window, gc::ResourceManager& resource_manager, const std::filesystem::path& open_file)
    : System(world), m_window(window), m_resource_manager(resource_manager)
{
    if (open_file.extension() == ".gcpak") {
        m_open_files.emplace_back(open_file);
    }
    else if (!open_file.empty()) {
        // a source file, or a directory of them
        m_pending_sources.push_back(open_file);
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
        if (std::filesystem::path(drag_drop_path).extension() == ".gcpak") {
            openGcpakFileDialogCallback(this, filelist.data(), 0);
        }
        else {
            // a source file, or a directory of them
            openAssetFileDialogCallback(this, filelist.data(), 0);
        }
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
        ImGui::Separator();
        // Shaders, textures and meshes are compiled into assets. glTF scenes are compiled into prefabs.
        if (ImGui::Button("Add Source Files")) {
            SDL_ShowOpenFileDialog(openAssetFileDialogCallback, this, m_window.getHandle(), m_asset_filters.data(), static_cast<int>(m_asset_filters.size()),
                                   NULL, true);
        }
        if (ImGui::Button("Add All Source Files In Folder")) {
            SDL_ShowOpenFolderDialog(openAssetFileDialogCallback, this, m_window.getHandle(), NULL, true);
        }
        if (!m_status.empty()) {
            ImGui::Separator();
            ImGui::TextUnformatted(m_status.c_str());
        }
    }
    ImGui::End();

    {
        std::unique_lock assets_lock(m_assets_mutex);

        processPendingSources();

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
                case gcpak::GcpakAssetType::TEXTURE_R8G8B8A8:
                case gcpak::GcpakAssetType::TEXTURE_R8G8B8A8_SRGB: {

                    ResourceTexture new_texture{};
                    new_texture.data = asset.asset.data;
                    new_texture.srgb = (asset.asset.type == gcpak::GcpakAssetType::TEXTURE_R8G8B8A8_SRGB);
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
                case gcpak::GcpakAssetType::PREFAB:
                    showPrefabPreview(asset.asset);
                    break;
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
    // (looked up every time, as pointers to components don't survive entities being created)
    m_world.getComponent<TransformComponent>(m_preview_entity)->setRotation(glm::angleAxis(angle, glm::vec3(0.0f, 0.0f, 1.0f)));
    if (m_prefab_pivot != ENTITY_NONE) {
        m_world.getComponent<TransformComponent>(m_prefab_pivot)->setRotation(glm::angleAxis(angle, glm::vec3(0.0f, 0.0f, 1.0f)));
    }
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

    // Compiling is left to the main thread, in processPendingSources()
    std::unique_lock lock(self->m_pending_sources_mutex);
    for (const char* const* file_path_ptr = filelist; *file_path_ptr; ++file_path_ptr) {
        self->m_pending_sources.emplace_back(*file_path_ptr);
    }
}

void EditorSystem::processPendingSources()
{
    std::vector<std::filesystem::path> sources{};
    {
        std::unique_lock lock(m_pending_sources_mutex);
        sources.swap(m_pending_sources);
    }
    if (sources.empty()) {
        return;
    }

    std::vector<Asset> compiled{};
    CompileStats total{};
    for (const auto& source : sources) {
        const CompileStats stats = compilePath(source, true, compiled);
        total.files_compiled += stats.files_compiled;
        total.files_failed += stats.files_failed;
    }

    // the selection refers to the lists that are about to change
    m_selected_asset_it = {};
    m_asset_being_previewed = nullptr;

    uint32_t replaced{};
    for (Asset& asset : compiled) {
        const uint32_t id = getAssetId(asset);
        for (auto& category : m_assets) {
            replaced += static_cast<uint32_t>(std::erase_if(category.second.assets, [id](const EditorAsset& existing) { return getAssetId(existing.asset) == id; }));
        }
        EditorAsset editor_asset{};
        editor_asset.asset = std::move(asset);
        editor_asset.from_file = &m_unsaved_file;
        m_assets[editor_asset.asset.type].assets.push_back(std::move(editor_asset));
    }

    m_status = std::format("Compiled {} file{}: {} asset{} ({} replaced)", total.files_compiled, total.files_compiled == 1 ? "" : "s", compiled.size(),
                           compiled.size() == 1 ? "" : "s", replaced);
    if (total.files_failed > 0) {
        m_status += std::format("\n{} file{} failed. See the log", total.files_failed, total.files_failed == 1 ? "" : "s");
    }
}

const Asset* EditorSystem::findAsset(gcpak::GcpakAssetType type, uint32_t id)
{
    const auto it = m_assets.find(type);
    if (it == m_assets.end()) {
        return nullptr;
    }
    for (const EditorAsset& editor_asset : it->second.assets) {
        if (getAssetId(editor_asset.asset) == id) {
            return &editor_asset.asset;
        }
    }
    return nullptr;
}

const Asset* EditorSystem::findTextureAsset(uint32_t id)
{
    const Asset* const asset = findAsset(gcpak::GcpakAssetType::TEXTURE_R8G8B8A8_SRGB, id);
    return asset ? asset : findAsset(gcpak::GcpakAssetType::TEXTURE_R8G8B8A8, id);
}

// The prefab's resources aren't in the content that the engine has loaded, they are assets in the editor. So for every mesh,
// material and texture that the prefab uses, a resource with the same name is made from the asset.
void EditorSystem::showPrefabPreview(const Asset& prefab)
{
    // Spins about the centre of the prefab: the pivot is rotated and scaled, and the offset moves the prefab's centre onto the pivot.
    m_prefab_pivot = m_world.createEntity("prefab_preview"_name, ENTITY_NONE, glm::vec3{0.0f, 5.0f, 0.0f});
    const Entity offset = m_world.createEntity("prefab_preview_offset"_name, m_prefab_pivot);

    std::vector<Entity> entities{};
    if (loadPrefab(prefab.data, m_world, offset, &entities) == ENTITY_NONE) {
        m_status = "Failed to load the prefab. See the log";
        clearPrefabPreview();
        return;
    }

    AABB bounds{glm::vec3{std::numeric_limits<float>::max()}, glm::vec3{std::numeric_limits<float>::lowest()}};
    bool has_bounds = false;
    for (const Entity entity : entities) {
        const RenderableComponent* const renderable = m_world.getComponent<RenderableComponent>(entity);
        if (!renderable || renderable->m_mesh.empty()) {
            continue;
        }
        const Name mesh_name = renderable->m_mesh;
        const Name material_name = renderable->m_material;
        const Asset* const mesh_asset = findAsset(gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16, mesh_name.getHash());
        if (!mesh_asset) {
            GC_WARN("Prefab {} uses a mesh that isn't open in the editor: {}", prefab.name, mesh_name);
            continue;
        }

        ResourceMesh mesh = createMeshFromData(mesh_asset->data);
        const AABB mesh_bounds = getAABBFromMesh(mesh);
        if (!m_resource_manager.add<ResourceMesh>(std::move(mesh), mesh_name).empty()) {
            m_prefab_meshes.push_back(mesh_name);
        }
        addPrefabPreviewMaterial(material_name);

        const glm::mat4 matrix = getMatrixRelativeTo(m_world, entity, offset);
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 local{(corner & 1) ? mesh_bounds.max.x : mesh_bounds.min.x, (corner & 2) ? mesh_bounds.max.y : mesh_bounds.min.y,
                                  (corner & 4) ? mesh_bounds.max.z : mesh_bounds.min.z};
            const glm::vec3 point = glm::vec3(matrix * glm::vec4(local, 1.0f));
            bounds.min = glm::min(bounds.min, point);
            bounds.max = glm::max(bounds.max, point);
            has_bounds = true;
        }
    }

    if (has_bounds) {
        const glm::vec3 size = bounds.max - bounds.min;
        const float largest = glm::max(size.x, glm::max(size.y, size.z));
        if (largest > 0.0f) {
            m_world.getComponent<TransformComponent>(m_prefab_pivot)->setScale(2.0f / largest);
        }
        m_world.getComponent<TransformComponent>(offset)->setPosition(-0.5f * (bounds.min + bounds.max));
    }
}

void EditorSystem::addPrefabPreviewMaterial(Name material_name)
{
    if (material_name.empty()) {
        return;
    }
    const Asset* const material_asset = findAsset(gcpak::GcpakAssetType::MATERIAL, material_name.getHash());
    if (!material_asset || material_asset->data.size() != 3 * sizeof(uint32_t)) {
        return;
    }
    std::array<uint32_t, 3> texture_ids{};
    std::memcpy(texture_ids.data(), material_asset->data.data(), material_asset->data.size());

    for (size_t i = 0; i < texture_ids.size(); ++i) {
        if (texture_ids[i] == 0) {
            continue;
        }
        const Asset* const texture_asset = findTextureAsset(texture_ids[i]);
        if (!texture_asset) {
            continue;
        }
        const bool srgb = (texture_asset->type == gcpak::GcpakAssetType::TEXTURE_R8G8B8A8_SRGB);
        if (!m_resource_manager.add<ResourceTexture>(ResourceTexture(texture_asset->data, srgb), Name(texture_ids[i])).empty()) {
            m_prefab_textures.push_back(Name(texture_ids[i]));
        }
    }

    ResourceMaterial material{};
    material.base_color_texture = Name(texture_ids[0]);
    material.orm_texture = Name(texture_ids[1]);
    material.normal_texture = Name(texture_ids[2]);
    if (!m_resource_manager.add<ResourceMaterial>(std::move(material), material_name).empty()) {
        m_prefab_materials.push_back(material_name);
    }
}

void EditorSystem::clearPrefabPreview()
{
    if (m_prefab_pivot != ENTITY_NONE) {
        m_world.deleteEntity(m_prefab_pivot); // and everything under it
        m_prefab_pivot = ENTITY_NONE;
    }
    for (const Name name : m_prefab_meshes) {
        m_resource_manager.deleteResource<ResourceMesh>(name);
    }
    for (const Name name : m_prefab_materials) {
        m_resource_manager.deleteResource<ResourceMaterial>(name);
    }
    for (const Name name : m_prefab_textures) {
        m_resource_manager.deleteResource<ResourceTexture>(name);
    }
    m_prefab_meshes.clear();
    m_prefab_materials.clear();
    m_prefab_textures.clear();
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
        if (asset.from_file->path.empty()) {
            ImGui::Text("From file: (not saved yet)");
        }
        else {
            ImGui::Text("From file: %s", asset.from_file->path.filename().string().c_str()); // FML
        }

        switch (asset.asset.type) {
        case gcpak::GcpakAssetType::TEXTURE_R8G8B8A8:
        case gcpak::GcpakAssetType::TEXTURE_R8G8B8A8_SRGB: {
            auto info = getAssetTextureInfo(asset.asset.data);
            ImGui::Text("Width: %u, Height: %u", info.width, info.height);
        } break;
        case gcpak::GcpakAssetType::MESH_POS12_NORM12_TANG16_UV8_INDEXED16: {
            auto info = getAssetMeshInfo(asset.asset.data);
            ImGui::Text("Vertices: %d, Triangles: %d", info.vertex_count, info.index_count / 3);
        } break;
        case gcpak::GcpakAssetType::MATERIAL: {
            if (asset.asset.data.size() == 3 * sizeof(uint32_t)) {
                std::array<uint32_t, 3> texture_ids{};
                std::memcpy(texture_ids.data(), asset.asset.data.data(), asset.asset.data.size());
                const std::array<const char*, 3> labels{"Base color", "Occlusion-roughness-metallic", "Normal"};
                for (size_t i = 0; i < texture_ids.size(); ++i) {
                    const Asset* const texture = findTextureAsset(texture_ids[i]);
                    if (texture_ids[i] == 0) {
                        ImGui::Text("%s: (none)", labels[i]);
                    }
                    else if (texture && !texture->name.empty()) {
                        ImGui::Text("%s: %s", labels[i], texture->name.c_str());
                    }
                    else {
                        ImGui::Text("%s: %#x", labels[i], texture_ids[i]);
                    }
                }
            }
        } break;
        case gcpak::GcpakAssetType::PREFAB: {
            const auto info = getAssetPrefabInfo(asset.asset.data);
            ImGui::Text("Entities: %u", info.entity_count);
            for (const auto& [component_name, count] : info.component_counts) {
                ImGui::Text("%s: %u", getComponentDisplayName(component_name).c_str(), count);
            }
            if (!info.valid) {
                ImGui::Text("The prefab is corrupt");
            }
        } break;
        default:
            break;
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
    clearPrefabPreview();

    // pointers to components don't survive entities being created, which previewing a prefab does
    m_preview_transform = m_world.getComponent<TransformComponent>(m_preview_entity);
    m_preview_renderable = m_world.getComponent<RenderableComponent>(m_preview_entity);

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
