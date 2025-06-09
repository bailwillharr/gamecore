#include "gamecore/gc_disk_io.h"

#include <filesystem>
#include <optional>

#include <SDL3/SDL_filesystem.h>

#include "gamecore/gc_logger.h"

namespace gc {

std::optional<std::filesystem::path> findContentDir()
{
    const char* const base_path = SDL_GetBasePath();
    if (base_path) {
        const std::filesystem::path content_dir = std::filesystem::path(base_path) / "content";
        if (std::filesystem::is_directory(content_dir)) {
            return content_dir;
        }
        else {
            GC_ERROR("Failed to find content dir: {} is not a directory", content_dir.string());
            return {};
        }
    }
    else {
        GC_ERROR("Failed to find content dir: SDL_GetBasePath() error: {}", SDL_GetError());
        return {};
    }
}

} // namespace gc
