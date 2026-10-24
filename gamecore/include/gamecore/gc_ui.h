#pragma once

// The game's UI: menus, HUDs and anything else that is part of the game, made with RmlUi (https://mikke89.github.io/RmlUiDoc/).
// ImGui is still there for debugging tools (see DebugUI), and is drawn on top.
//
// A UI is laid out in documents (.rml, much like HTML) styled by style sheets (.rcss, much like CSS). They go in content/ui, and
// are packaged into content/ui.gcpak by the build. Like every asset, a file is found by its name alone (the directories it is in
// make no difference), so a document links a style sheet as <link type="text/rcss" href="hud.rcss"/>, whatever directory it is in.
// While content/ui exists on disk (in a development build), files are read from there instead, so that a document can be edited
// and reloaded while the game runs (F10, Windows > UI).
//
// Usually a document is shown by giving an entity a UIComponent (see gc_ui_system.h). Games put values into documents with RmlUi's
// data bindings: create a data model on getContext() before the document is loaded, and bind variables to it. The document then
// shows them with {{ }}, data-for, data-class-*, data-style-* and so on.
//
// Sizes: the context is the size of the window in pixels. Lay documents out in 'dp' (and %, vw, vh) rather than 'px', and they are
// the same size relative to the screen at any resolution: a dp is getDpRatio() pixels. See UIScaling.
//
// Fonts: Lato (regular, bold, italic and bold italic) is always loaded, as the font-family "LatoLatin". Load others with loadFontFace().
// ui_base.rcss gives the HTML-like elements (div, p, table...) the display they are expected to have. Link it before your own sheets.
//
// What isn't supported by the renderer: filters (filter, backdrop-filter), box-shadow and mask-image. They are left out.

#include <cstdint>

#include <filesystem>
#include <memory>
#include <string>

#include <glm/vec2.hpp>

#include "gamecore/gc_name.h"

struct SDL_Window; // forward-dec
union SDL_Event;   // forward-dec

namespace Rml {
class Context;         // forward-dec
class ElementDocument; // forward-dec
} // namespace Rml

namespace gc {

class Content;           // forward-dec
class RenderBackend;     // forward-dec
class UIDrawData;        // forward-dec
class UIRenderInterface; // forward-dec
class UISystemInterface; // forward-dec
class UIFileInterface;   // forward-dec

enum class UIScaleMode {
    // The larger of the two below. A fullscreen game looks the same at any resolution, and a UI in a small window is never smaller
    // than the desktop's own text.
    AUTO,
    // The display's scale: its DPI setting, such as 1.5 for 150%
    DISPLAY,
    // The window's height / reference_height. The UI is always the same fraction of the window
    WINDOW_HEIGHT,
};

// How many pixels a dp is (Rml::Context::SetDensityIndependentPixelRatio()). Recalculated whenever the window changes.
struct UIScaling {
    UIScaleMode mode = UIScaleMode::AUTO;
    float reference_height = 1080.0f; // the window height (in pixels) at which a dp is a pixel
    float user_scale = 1.0f;          // multiplies the result: for a player's setting
};

class UI {
    SDL_Window* const m_window;
    std::unique_ptr<UISystemInterface> m_system_interface;
    std::unique_ptr<UIFileInterface> m_file_interface;
    std::unique_ptr<UIRenderInterface> m_render_interface;
    Rml::Context* m_context{};

    UIScaling m_scaling{};
    float m_dp_ratio{1.0f};
    glm::ivec2 m_size{};

    uint32_t m_reload_count{};
    bool m_debugger_visible{};

public:
    // source_directory: where the UI's files are, unpackaged (content/ui), if it exists
    UI(SDL_Window* window, RenderBackend& render_backend, Content& content, const std::filesystem::path& source_directory);
    UI(const UI&) = delete;
    UI(UI&&) = delete;

    ~UI();

    UI& operator=(const UI&) = delete;
    UI& operator=(UI&&) = delete;

    // The RmlUi context that every document is in. Create data models on it, before the documents that use them are loaded.
    Rml::Context& getContext();

    void setScaling(const UIScaling& scaling);
    const UIScaling& getScaling() const;
    // The pixels in a dp
    float getDpRatio() const;
    // The size of the context: the window's, in pixels
    glm::ivec2 getSize() const;

    // Loads a document (hidden), named as its asset is: the file's name, such as "hud.rml". Returns null if it failed (which is logged).
    // Close it with Rml::ElementDocument::Close(). UISystem does all this for documents that entities have.
    Rml::ElementDocument* loadDocument(Name name);

    // Loads a font (a .ttf or .otf asset). Its family, style and weight are read from the font. A fallback face is used for
    // characters that the other fonts don't have.
    bool loadFontFace(Name name, bool fallback = false);

    // Has the style sheets, documents and images read again: the documents of UIComponents are loaded again, and anything else
    // that loaded a document should do so when getReloadCount() changes.
    void reloadDocuments();
    uint32_t getReloadCount() const;

    // Gives the event to RmlUi, and hides it from the game if RmlUi used it (by setting its type to zero).
    // While the mouse is captured, the game has the mouse and RmlUi isn't told about it.
    void processEvent(SDL_Event& ev, bool mouse_captured);

    // Call every frame after the world's update: updates the documents, and lays them out and draws them into draw_data
    void update(UIDrawData& draw_data);

    // The debug window (F10, Windows > UI). open is from DebugUI::getWindowOpen()
    void drawDebugWindow(bool* open);
};

} // namespace gc
