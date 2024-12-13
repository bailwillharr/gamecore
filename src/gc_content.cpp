#include "gamecore/gc_content.h"

#include <filesystem>

#include "gamecore/gc_abort.h"
#include "gamecore/gc_disk_io.h"

namespace gc {

Content::Content() : m_content_dir(findContentDir())
{
    if (m_content_dir.empty()) {
        abortGame("Failed to find content directory");
    }
    // Get a list of the directories in content/ and the archives.
    // Each will contains a manifest file mapping crc32 ids to relative paths.
    // Separate manifests cannot have conflicting ids
    // (i.e., different dirs/archives cannot have duplicate file names)
}

Content::~Content() {}

} // namespace gc