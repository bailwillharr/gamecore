#pragma once

// The interfaces that RmlUi gets the time, logging, the cursor and files through (see gc_ui.h). Private to the engine.

#include <cstdint>

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include "gamecore/gc_name.h"

struct SDL_Window; // forward-dec
struct SDL_Cursor; // forward-dec

namespace gc {

class Content; // forward-dec

class UISystemInterface final : public Rml::SystemInterface {
    enum class Cursor { ARROW, MOVE, POINTER, RESIZE, CROSS, TEXT, UNAVAILABLE, COUNT };

    SDL_Window* const m_window;
    std::array<SDL_Cursor*, static_cast<size_t>(Cursor::COUNT)> m_cursors{};

public:
    explicit UISystemInterface(SDL_Window* window);
    UISystemInterface(const UISystemInterface&) = delete;
    UISystemInterface(UISystemInterface&&) = delete;

    ~UISystemInterface() override;

    UISystemInterface& operator=(const UISystemInterface&) = delete;
    UISystemInterface& operator=(UISystemInterface&&) = delete;

    double GetElapsedTime() override;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
    void SetMouseCursor(const Rml::String& cursor_name) override;
    void SetClipboardText(const Rml::String& text) override;
    void GetClipboardText(Rml::String& text) override;
    void ActivateKeyboard(Rml::Vector2f caret_position, float line_height) override;
    void DeactivateKeyboard() override;
};

// Every file is found by its name alone, as an asset in the Content, or in the source directory (searched through, with the
// directories in it) if there is one, which comes first.
class UIFileInterface final : public Rml::FileInterface {
    struct OpenFile {
        std::vector<uint8_t> owned; // what was read from the source directory
        std::span<const uint8_t> data;
        size_t position;
    };

    Content& m_content;
    std::filesystem::path m_source_directory;
    std::unordered_map<Name, std::filesystem::path> m_source_files{};

public:
    UIFileInterface(Content& content, const std::filesystem::path& source_directory);
    UIFileInterface(const UIFileInterface&) = delete;
    UIFileInterface(UIFileInterface&&) = delete;

    ~UIFileInterface() override;

    UIFileInterface& operator=(const UIFileInterface&) = delete;
    UIFileInterface& operator=(UIFileInterface&&) = delete;

    // The asset that a path means: the file's name, or "0x" and the hex of an asset ID (what Name::getString() gives for a Name it
    // doesn't know the string of)
    static Name getAssetName(std::string_view path);

    // Looks through the source directory again, for files that have been added
    void scanSourceDirectory();
    bool hasSourceDirectory() const;

    // Reads an image: a .png or .jpg in the source directory, or a texture asset. Gives RGBA8 texels (sRGB, alpha not premultiplied)
    bool loadImage(const std::string& path, std::vector<uint8_t>& texels, uint32_t& width, uint32_t& height);

    Rml::FileHandle Open(const Rml::String& path) override;
    void Close(Rml::FileHandle file) override;
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override;
    bool Seek(Rml::FileHandle file, long offset, int origin) override;
    size_t Tell(Rml::FileHandle file) override;
    size_t Length(Rml::FileHandle file) override;
};

} // namespace gc
