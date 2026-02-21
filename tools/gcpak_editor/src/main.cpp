#include <SDL3/SDL_main.h>

#include "gamecore/gc_abort.h"
#include "gcpak_editor.h"

#include <filesystem>
#include <iostream>
#include <trigonometric.hpp>
#include <vector>
#include <format>
#include <system_error>

#include <stb_image.h>

#include <gcpak/gcpak.h>
#include <gcpak/gcpak_prefab.h>

#include <gamecore/gc_logger.h>
#include <gamecore/gc_app.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_render_backend.h>
#include <gamecore/gc_world.h>
#include <gamecore/gc_renderable_component.h>
#include <gamecore/gc_camera_component.h>
#include <gamecore/gc_light_component.h>
#include <gamecore/gc_render_system.h>
#include <gamecore/gc_camera_system.h>
#include <gamecore/gc_light_system.h>
#include <gamecore/gc_resource_manager.h>
#include <gamecore/gc_gen_mesh.h>

#include "editor_system.h"

template <typename Enum>
static constexpr std::underlying_type_t<Enum> to_underlying(Enum e) noexcept
{
    return static_cast<std::underlying_type_t<Enum>>(e);
}

static bool printcontent()
{
    std::error_code ec;
    const auto content_dir = std::filesystem::path(GCPAK_EDITOR_SOURCE_DIRECTORY).parent_path().parent_path() / "content";
    if (!std::filesystem::exists(content_dir, ec) || !std::filesystem::is_directory(content_dir, ec)) {
        std::cerr << "Failed to find content directory! error: " << ec.message() << "\n";
        return false;
    }

    const auto gcpak_path = content_dir / "meshes.gcpak";

    gcpak::GcpakCreator creator(gcpak_path);

    if (auto error = creator.getError(); error.has_value()) {
        GC_ERROR("FILE ERROR: {}", error.value());
        return false;
    }

    for (const auto& asset : creator.getAssets()) {
        GC_INFO("ASSET");
        GC_INFO("    name: {}", asset.name);
        GC_INFO("    hash: {}", asset.hash);
        GC_INFO("    type: {}", to_underlying(asset.type));
        GC_INFO("    data size: {}", asset.data.size());
    }

    return true;
}

static void initEditorWorld(gc::App& app)
{
    using namespace gc;
    using namespace gc::literals;

    auto& world = app.world();
    auto& resource_manager = app.resourceManager();
    auto& render_backend = app.renderBackend();
    auto& content_manager = app.content();

    {
        // set up pipeline
        auto vertex_spv = content_manager.findAsset("fancy.vert"_name, gcpak::GcpakAssetType::SPIRV_SHADER);
        auto fragment_spv = content_manager.findAsset("fancy.frag"_name, gcpak::GcpakAssetType::SPIRV_SHADER);
        if (vertex_spv.empty() || fragment_spv.empty()) {
            abortGame("Failed to load vertex or fragment shader");
        }
        render_backend.createPipeline(vertex_spv, fragment_spv);
    }

    world.registerComponent<gc::RenderableComponent, gc::ComponentArrayType::DENSE>();
    world.registerComponent<gc::CameraComponent, gc::ComponentArrayType::SPARSE>();
    world.registerComponent<gc::LightComponent, gc::ComponentArrayType::SPARSE>();

    world.registerSystem<gc::RenderSystem>(resource_manager, render_backend);
    world.registerSystem<gc::CameraSystem>();
    world.registerSystem<gc::LightSystem>();

    world.registerSystem<EditorSystem>();

    {
        resource_manager.add(genCubeMesh(), "cube"_name);
    }

    {
        auto camera = world.createEntity("camera"_name);
        world.getComponent<TransformComponent>(camera)->setPosition(0, 0, 10);
        world.addComponent<CameraComponent>(camera);
    }

    {
        auto cube = world.createEntity("cube"_name);
        world.addComponent<RenderableComponent>(cube).setMesh("cube"_name);
    }
}

int main(int, char*[])
{
    gc::AppInitOptions options{};
    options.name = "gcpak_editor";
    options.version = "v0.1.0";
    options.author = "bailwillharr";

    // Only load the shaders file, to allow the other pak files to be written.
    options.pak_files_override.push_back("shaders.gcpak");

    gc::App::initialise(options);

    auto& app = gc::App::instance();

    initEditorWorld(app);

    auto& render_backend = app.renderBackend();
    render_backend.setSyncMode(gc::RenderSyncMode::VSYNC_ON_DOUBLE_BUFFERED);

    auto& window = app.window();
    window.setTitle("Gcpak Editor");
    window.setIsResizable(true);
    window.setWindowVisibility(true);

    app.run();

    gc::App::shutdown();

    return 0;
}
