#include <SDL3/SDL_main.h>

#include "gcpak_editor.h"

#include <cstdint>

#include <atomic>
#include <array>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <map>
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

template <typename Enum>
static constexpr std::underlying_type_t<Enum> to_underlying(Enum e) noexcept
{
    return static_cast<std::underlying_type_t<Enum>>(e);
}

int main(int, char*[])
{
    std::error_code ec;
    const auto content_dir = std::filesystem::path(GCPAK_EDITOR_SOURCE_DIRECTORY).parent_path().parent_path() / "content";
    if (!std::filesystem::exists(content_dir, ec) || !std::filesystem::is_directory(content_dir, ec)) {
        std::cerr << "Failed to find content directory! error: " << ec.message() << "\n";
        return EXIT_FAILURE;
    }

    const auto gcpak_path = content_dir / "meshes.gcpak";

    gcpak::GcpakCreator creator(gcpak_path);

    if (auto error = creator.getError(); error.has_value()) {
        GC_ERROR("FILE ERROR: {}", error.value());
        return -1;
    }

    for (const auto& asset : creator.getAssets()) {
        GC_INFO("ASSET");
        GC_INFO("    name: {}", asset.name);
        GC_INFO("    hash: {}", asset.hash);
        GC_INFO("    type: {}", to_underlying(asset.type));
        GC_INFO("    data size: {}", asset.data.size());
    }

    gc::AppInitOptions options{};
    options.name = "gcpak_editor";
    options.version = "v0.1.0";
    options.author = "bailwillharr";

    gc::App::initialise(options);

    auto& app = gc::App::instance();
    auto& window = app.window();
    auto& render_backend = app.renderBackend();

    render_backend.setSyncMode(gc::RenderSyncMode::VSYNC_ON_DOUBLE_BUFFERED);

    window.setTitle("Gcpak Editor");
    window.setIsResizable(true);
    window.setWindowVisibility(true);

    app.run();

    gc::App::shutdown();

    return 0;
}