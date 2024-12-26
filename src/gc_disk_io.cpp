#include "gamecore/gc_disk_io.h"

#include <cstdlib>

#include <filesystem>
#include <optional>

#ifdef WIN32
#include <Windows.h>
#endif

#include "gamecore/gc_assert.h"
#include "gamecore/gc_logger.h"

namespace gc {

std::optional<std::filesystem::path> findContentDir()
{
#ifdef WIN32
    // get resource path relative to exe directory if on windows

    // Copied from github.com/libsdl-org/SDL
    // src/filesystem/windows/SDL_sysfilesystem.c
    // SDL_SYS_GetBasePath(void)

    DWORD buflen = 128;
    WCHAR* path = NULL;
    std::filesystem::path content_dir{};
    DWORD len = 0;
    int i;

    while (true) {
        void* ptr = realloc(path, buflen * sizeof(WCHAR));
        if (!ptr) {
            free(path);
            return {};
        }

        path = (WCHAR*)ptr;

        len = GetModuleFileNameW(NULL, path, buflen);
        // if it truncated, then len >= buflen - 1
        // if there was enough room (or failure), len < buflen - 1
        if (len < buflen - 1) {
            break;
        }

        // buffer too small? Try again.
        buflen *= 2;
    }

    if (len == 0) {
        free(path);
        GC_ERROR("Couldn't locate our .exe");
        return {};
    }

    for (i = len - 1; i > 0; i--) {
        if (path[i] == '\\') {
            break;
        }
    }

    GC_ASSERT(i > 0);   // Should have been an absolute path.
    path[i + 1] = '\0'; // chop off filename.

    content_dir = std::filesystem::path(path);
    free(path);
#else
    // for other OS's use current directory ._.
    std::filesystem::path content_dir = std::filesystem::current_path();
#endif
    content_dir /= "content";
    if (std::filesystem::is_directory(content_dir) == false) {
        GC_ERROR("Unable to find game resources directory");
        return {};
    }
    else {
        return content_dir;
    }
}

} // namespace gc