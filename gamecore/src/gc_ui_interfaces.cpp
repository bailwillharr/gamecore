#include "gc_ui_interfaces.h"

#include <cstdio>
#include <cstring>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <system_error>

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <stb_image.h>

#include <gcpak/gcpak.h>

#include "gamecore/gc_content.h"
#include "gclog/gclog.h"

namespace gc {

//
// UISystemInterface
//

UISystemInterface::UISystemInterface(SDL_Window* window) : m_window(window)
{
    m_cursors[static_cast<size_t>(Cursor::ARROW)] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    m_cursors[static_cast<size_t>(Cursor::MOVE)] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE);
    m_cursors[static_cast<size_t>(Cursor::POINTER)] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    m_cursors[static_cast<size_t>(Cursor::RESIZE)] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NWSE_RESIZE);
    m_cursors[static_cast<size_t>(Cursor::CROSS)] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
    m_cursors[static_cast<size_t>(Cursor::TEXT)] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    m_cursors[static_cast<size_t>(Cursor::UNAVAILABLE)] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NOT_ALLOWED);
}

UISystemInterface::~UISystemInterface()
{
    for (SDL_Cursor* cursor : m_cursors) {
        if (cursor) {
            SDL_DestroyCursor(cursor);
        }
    }
}

double UISystemInterface::GetElapsedTime() { return static_cast<double>(SDL_GetTicksNS()) * 1e-9; }

bool UISystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message)
{
    switch (type) {
    case Rml::Log::LT_ERROR:
    case Rml::Log::LT_ASSERT:
        GC_ERROR("RmlUi: {}", message);
        break;
    case Rml::Log::LT_WARNING:
        GC_WARN("RmlUi: {}", message);
        break;
    case Rml::Log::LT_DEBUG:
        GC_DEBUG("RmlUi: {}", message);
        break;
    default:
        GC_INFO("RmlUi: {}", message);
        break;
    }
    return true; // (false would break into the debugger on an RmlUi assert)
}

void UISystemInterface::SetMouseCursor(const Rml::String& cursor_name)
{
    Cursor cursor = Cursor::COUNT;
    if (cursor_name.empty() || cursor_name == "arrow") {
        cursor = Cursor::ARROW;
    }
    else if (cursor_name == "move" || cursor_name.starts_with("rmlui-scroll")) {
        cursor = Cursor::MOVE;
    }
    else if (cursor_name == "pointer") {
        cursor = Cursor::POINTER;
    }
    else if (cursor_name == "resize") {
        cursor = Cursor::RESIZE;
    }
    else if (cursor_name == "cross") {
        cursor = Cursor::CROSS;
    }
    else if (cursor_name == "text") {
        cursor = Cursor::TEXT;
    }
    else if (cursor_name == "unavailable") {
        cursor = Cursor::UNAVAILABLE;
    }
    if (cursor != Cursor::COUNT && m_cursors[static_cast<size_t>(cursor)]) {
        SDL_SetCursor(m_cursors[static_cast<size_t>(cursor)]);
    }
}

void UISystemInterface::SetClipboardText(const Rml::String& text) { SDL_SetClipboardText(text.c_str()); }

void UISystemInterface::GetClipboardText(Rml::String& text)
{
    char* const clipboard = SDL_GetClipboardText();
    text = clipboard ? Rml::String(clipboard) : Rml::String();
    SDL_free(clipboard);
}

void UISystemInterface::ActivateKeyboard(Rml::Vector2f caret_position, float line_height)
{
    // (where an input method editor's window goes)
    const SDL_Rect rect{static_cast<int>(caret_position.x), static_cast<int>(caret_position.y), 1, static_cast<int>(line_height)};
    SDL_SetTextInputArea(m_window, &rect, 0);
    SDL_StartTextInput(m_window);
}

void UISystemInterface::DeactivateKeyboard() { SDL_StopTextInput(m_window); }

//
// UIFileInterface
//

UIFileInterface::UIFileInterface(Content& content, const std::filesystem::path& source_directory)
    : m_content(content), m_source_directory(source_directory)
{
    scanSourceDirectory();
}

UIFileInterface::~UIFileInterface() = default;

Name UIFileInterface::getAssetName(std::string_view path)
{
    const size_t slash = path.find_last_of("/\\");
    const std::string_view file_name = (slash == std::string_view::npos) ? path : path.substr(slash + 1);
    if (file_name.size() > 2 && file_name.starts_with("0x")) {
        uint32_t hash{};
        const char* const end = file_name.data() + file_name.size();
        if (const auto [ptr, ec] = std::from_chars(file_name.data() + 2, end, hash, 16); ec == std::errc{} && ptr == end) {
            return Name(hash);
        }
    }
    return Name(std::string(file_name));
}

void UIFileInterface::scanSourceDirectory()
{
    m_source_files.clear();
    std::error_code ec{};
    if (m_source_directory.empty() || !std::filesystem::is_directory(m_source_directory, ec)) {
        return;
    }
    for (std::filesystem::recursive_directory_iterator it(m_source_directory, std::filesystem::directory_options::skip_permission_denied, ec), end{};
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            const Name name(it->path().filename().string());
            if (const auto [existing, inserted] = m_source_files.emplace(name, it->path()); !inserted) {
                GC_WARN("UI: {} and {} have the same name. Only the first can be used", existing->second.string(), it->path().string());
            }
        }
    }
}

bool UIFileInterface::hasSourceDirectory() const { return !m_source_files.empty(); }

static bool readFile(const std::filesystem::path& path, std::vector<uint8_t>& data)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool UIFileInterface::loadImage(const std::string& path, std::vector<uint8_t>& texels, uint32_t& width, uint32_t& height)
{
    const Name name = getAssetName(path);

    if (const auto it = m_source_files.find(name); it != m_source_files.end()) {
        std::vector<uint8_t> file{};
        if (!readFile(it->second, file)) {
            GC_ERROR("UI: failed to read {}", it->second.string());
            return false;
        }
        int w{}, h{}, components{};
        stbi_uc* const pixels = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &w, &h, &components, 4);
        if (!pixels) {
            GC_ERROR("UI: failed to decode {}: {}", it->second.string(), stbi_failure_reason());
            return false;
        }
        width = static_cast<uint32_t>(w);
        height = static_cast<uint32_t>(h);
        texels.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
        stbi_image_free(pixels);
        return true;
    }

    const AssetView asset = m_content.findAsset(name);
    if (asset.data.empty()) {
        GC_ERROR("UI: there is no image named {}", path);
        return false;
    }
    if (asset.type != gcpak::GcpakAssetType::TEXTURE_R8G8B8A8_SRGB && asset.type != gcpak::GcpakAssetType::TEXTURE_R8G8B8A8) {
        GC_ERROR("UI: {} isn't a texture", path);
        return false;
    }
    if (asset.data.size() < 2 * sizeof(uint32_t)) {
        return false;
    }
    std::memcpy(&width, asset.data.data(), sizeof(uint32_t));
    std::memcpy(&height, asset.data.data() + sizeof(uint32_t), sizeof(uint32_t));
    const size_t size = static_cast<size_t>(width) * height * 4;
    if (width == 0 || height == 0 || asset.data.size() < 2 * sizeof(uint32_t) + size) {
        GC_ERROR("UI: the texture {} is the wrong size", path);
        return false;
    }
    const uint8_t* const first = asset.data.data() + 2 * sizeof(uint32_t);
    texels.assign(first, first + size);
    return true;
}

Rml::FileHandle UIFileInterface::Open(const Rml::String& path)
{
    const Name name = getAssetName(path);

    if (const auto it = m_source_files.find(name); it != m_source_files.end()) {
        auto* const file = new OpenFile{};
        if (!readFile(it->second, file->owned)) {
            GC_ERROR("UI: failed to read {}", it->second.string());
            delete file;
            return {};
        }
        file->data = file->owned;
        return reinterpret_cast<Rml::FileHandle>(file);
    }

    const AssetView asset = m_content.findAsset(name);
    if (asset.data.empty()) {
        // (RmlUi logs that it couldn't open the file, with the path it was looking for)
        return {};
    }
    if (asset.type != gcpak::GcpakAssetType::RAW) {
        GC_ERROR("UI: {} is an asset that isn't a file", path);
        return {};
    }
    auto* const file = new OpenFile{};
    file->data = asset.data; // (the Content's data stays mapped)
    return reinterpret_cast<Rml::FileHandle>(file);
}

void UIFileInterface::Close(Rml::FileHandle file) { delete reinterpret_cast<OpenFile*>(file); }

size_t UIFileInterface::Read(void* buffer, size_t size, Rml::FileHandle file)
{
    OpenFile& f = *reinterpret_cast<OpenFile*>(file);
    const size_t count = std::min(size, f.data.size() - f.position);
    std::memcpy(buffer, f.data.data() + f.position, count);
    f.position += count;
    return count;
}

bool UIFileInterface::Seek(Rml::FileHandle file, long offset, int origin)
{
    OpenFile& f = *reinterpret_cast<OpenFile*>(file);
    long long position{};
    switch (origin) {
    case SEEK_SET:
        position = offset;
        break;
    case SEEK_CUR:
        position = static_cast<long long>(f.position) + offset;
        break;
    case SEEK_END:
        position = static_cast<long long>(f.data.size()) + offset;
        break;
    default:
        return false;
    }
    if (position < 0 || position > static_cast<long long>(f.data.size())) {
        return false;
    }
    f.position = static_cast<size_t>(position);
    return true;
}

size_t UIFileInterface::Tell(Rml::FileHandle file) { return reinterpret_cast<OpenFile*>(file)->position; }

size_t UIFileInterface::Length(Rml::FileHandle file) { return reinterpret_cast<OpenFile*>(file)->data.size(); }

} // namespace gc
