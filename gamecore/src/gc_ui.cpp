#include "gamecore/gc_ui.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_video.h>

#include <imgui.h>

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>

#include <tracy/Tracy.hpp>

#include "gamecore/gc_abort.h"
#include "gamecore/gc_content.h"
#include "gamecore/gc_render_backend.h"
#include "gamecore/gc_ui_draw_data.h"
#include "gc_ui_interfaces.h"
#include "gc_ui_render_interface.h"
#include "gclog/gclog.h"

namespace gc {

using namespace gc::literals;

// The fonts that are always there (content/ui/fonts). The first is the fallback for characters that the others haven't got
static constexpr std::array<const char*, 4> DEFAULT_FONTS{"LatoLatin-Regular.ttf", "LatoLatin-Bold.ttf", "LatoLatin-Italic.ttf",
                                                          "LatoLatin-BoldItalic.ttf"};

// (from RmlUi's SDL backend, RmlUi_Platform_SDL.cpp)
static Rml::Input::KeyIdentifier convertKey(SDL_Keycode key)
{
    using namespace Rml::Input;
    switch (key) {
    case SDLK_UNKNOWN: return KI_UNKNOWN;
    case SDLK_ESCAPE: return KI_ESCAPE;
    case SDLK_SPACE: return KI_SPACE;
    case SDLK_0: return KI_0;
    case SDLK_1: return KI_1;
    case SDLK_2: return KI_2;
    case SDLK_3: return KI_3;
    case SDLK_4: return KI_4;
    case SDLK_5: return KI_5;
    case SDLK_6: return KI_6;
    case SDLK_7: return KI_7;
    case SDLK_8: return KI_8;
    case SDLK_9: return KI_9;
    case SDLK_A: return KI_A;
    case SDLK_B: return KI_B;
    case SDLK_C: return KI_C;
    case SDLK_D: return KI_D;
    case SDLK_E: return KI_E;
    case SDLK_F: return KI_F;
    case SDLK_G: return KI_G;
    case SDLK_H: return KI_H;
    case SDLK_I: return KI_I;
    case SDLK_J: return KI_J;
    case SDLK_K: return KI_K;
    case SDLK_L: return KI_L;
    case SDLK_M: return KI_M;
    case SDLK_N: return KI_N;
    case SDLK_O: return KI_O;
    case SDLK_P: return KI_P;
    case SDLK_Q: return KI_Q;
    case SDLK_R: return KI_R;
    case SDLK_S: return KI_S;
    case SDLK_T: return KI_T;
    case SDLK_U: return KI_U;
    case SDLK_V: return KI_V;
    case SDLK_W: return KI_W;
    case SDLK_X: return KI_X;
    case SDLK_Y: return KI_Y;
    case SDLK_Z: return KI_Z;
    case SDLK_SEMICOLON: return KI_OEM_1;
    case SDLK_PLUS: return KI_OEM_PLUS;
    case SDLK_COMMA: return KI_OEM_COMMA;
    case SDLK_MINUS: return KI_OEM_MINUS;
    case SDLK_PERIOD: return KI_OEM_PERIOD;
    case SDLK_SLASH: return KI_OEM_2;
    case SDLK_GRAVE: return KI_OEM_3;
    case SDLK_LEFTBRACKET: return KI_OEM_4;
    case SDLK_BACKSLASH: return KI_OEM_5;
    case SDLK_RIGHTBRACKET: return KI_OEM_6;
    case SDLK_DBLAPOSTROPHE: return KI_OEM_7;
    case SDLK_KP_0: return KI_NUMPAD0;
    case SDLK_KP_1: return KI_NUMPAD1;
    case SDLK_KP_2: return KI_NUMPAD2;
    case SDLK_KP_3: return KI_NUMPAD3;
    case SDLK_KP_4: return KI_NUMPAD4;
    case SDLK_KP_5: return KI_NUMPAD5;
    case SDLK_KP_6: return KI_NUMPAD6;
    case SDLK_KP_7: return KI_NUMPAD7;
    case SDLK_KP_8: return KI_NUMPAD8;
    case SDLK_KP_9: return KI_NUMPAD9;
    case SDLK_KP_ENTER: return KI_NUMPADENTER;
    case SDLK_KP_MULTIPLY: return KI_MULTIPLY;
    case SDLK_KP_PLUS: return KI_ADD;
    case SDLK_KP_MINUS: return KI_SUBTRACT;
    case SDLK_KP_PERIOD: return KI_DECIMAL;
    case SDLK_KP_DIVIDE: return KI_DIVIDE;
    case SDLK_KP_EQUALS: return KI_OEM_NEC_EQUAL;
    case SDLK_BACKSPACE: return KI_BACK;
    case SDLK_TAB: return KI_TAB;
    case SDLK_CLEAR: return KI_CLEAR;
    case SDLK_RETURN: return KI_RETURN;
    case SDLK_PAUSE: return KI_PAUSE;
    case SDLK_CAPSLOCK: return KI_CAPITAL;
    case SDLK_PAGEUP: return KI_PRIOR;
    case SDLK_PAGEDOWN: return KI_NEXT;
    case SDLK_END: return KI_END;
    case SDLK_HOME: return KI_HOME;
    case SDLK_LEFT: return KI_LEFT;
    case SDLK_UP: return KI_UP;
    case SDLK_RIGHT: return KI_RIGHT;
    case SDLK_DOWN: return KI_DOWN;
    case SDLK_INSERT: return KI_INSERT;
    case SDLK_DELETE: return KI_DELETE;
    case SDLK_HELP: return KI_HELP;
    case SDLK_F1: return KI_F1;
    case SDLK_F2: return KI_F2;
    case SDLK_F3: return KI_F3;
    case SDLK_F4: return KI_F4;
    case SDLK_F5: return KI_F5;
    case SDLK_F6: return KI_F6;
    case SDLK_F7: return KI_F7;
    case SDLK_F8: return KI_F8;
    case SDLK_F9: return KI_F9;
    case SDLK_F10: return KI_F10;
    case SDLK_F11: return KI_F11;
    case SDLK_F12: return KI_F12;
    case SDLK_NUMLOCKCLEAR: return KI_NUMLOCK;
    case SDLK_SCROLLLOCK: return KI_SCROLL;
    case SDLK_LSHIFT: return KI_LSHIFT;
    case SDLK_RSHIFT: return KI_RSHIFT;
    case SDLK_LCTRL: return KI_LCONTROL;
    case SDLK_RCTRL: return KI_RCONTROL;
    case SDLK_LALT: return KI_LMENU;
    case SDLK_RALT: return KI_RMENU;
    case SDLK_LGUI: return KI_LMETA;
    case SDLK_RGUI: return KI_RMETA;
    default: return KI_UNKNOWN;
    }
}

static int getKeyModifiers()
{
    const SDL_Keymod mods = SDL_GetModState();
    int result = 0;
    if (mods & SDL_KMOD_CTRL) {
        result |= Rml::Input::KM_CTRL;
    }
    if (mods & SDL_KMOD_SHIFT) {
        result |= Rml::Input::KM_SHIFT;
    }
    if (mods & SDL_KMOD_ALT) {
        result |= Rml::Input::KM_ALT;
    }
    if (mods & SDL_KMOD_NUM) {
        result |= Rml::Input::KM_NUMLOCK;
    }
    if (mods & SDL_KMOD_CAPS) {
        result |= Rml::Input::KM_CAPSLOCK;
    }
    return result;
}

static int convertMouseButton(uint8_t button)
{
    switch (button) {
    case SDL_BUTTON_LEFT:
        return 0;
    case SDL_BUTTON_RIGHT:
        return 1;
    case SDL_BUTTON_MIDDLE:
        return 2;
    default:
        return 3;
    }
}

UI::UI(SDL_Window* window, RenderBackend& render_backend, Content& content, const std::filesystem::path& source_directory) : m_window(window)
{
    m_system_interface = std::make_unique<UISystemInterface>(window);
    m_file_interface = std::make_unique<UIFileInterface>(content, source_directory);
    m_render_interface = std::make_unique<UIRenderInterface>(render_backend, *m_file_interface);
    if (m_file_interface->hasSourceDirectory()) {
        GC_INFO("UI files are read from {}, not from the packages", source_directory.string());
    }

    Rml::SetSystemInterface(m_system_interface.get());
    Rml::SetFileInterface(m_file_interface.get());
    Rml::SetRenderInterface(m_render_interface.get());
    if (!Rml::Initialise()) {
        abortGame("Failed to initialise RmlUi");
    }

    // The shaders are in content/shaders.gcpak with the rest
    const AssetView vert = content.findAsset("ui.vert"_name);
    const AssetView frag = content.findAsset("ui.frag"_name);
    if (vert.type == gcpak::GcpakAssetType::SPIRV_SHADER && frag.type == gcpak::GcpakAssetType::SPIRV_SHADER) {
        render_backend.setUIShaders(vert.data, frag.data);
    }
    else {
        GC_WARN("The UI's shaders (ui.vert, ui.frag) aren't in the content, so the UI won't be drawn. Package content/shader_src again");
    }

    int width{}, height{};
    SDL_GetWindowSizeInPixels(window, &width, &height);
    m_size = glm::ivec2(std::max(width, 1), std::max(height, 1));
    m_context = Rml::CreateContext("main", Rml::Vector2i(m_size.x, m_size.y));
    if (!m_context) {
        abortGame("Failed to create the RmlUi context");
    }

    for (size_t i = 0; i < DEFAULT_FONTS.size(); ++i) {
        if (!loadFontFace(Name(DEFAULT_FONTS[i]), i == 0)) {
            GC_WARN("Failed to load the UI's font {}. Is content/ui.gcpak missing?", DEFAULT_FONTS[i]);
        }
    }

    Rml::Debugger::Initialise(m_context);
    Rml::Debugger::SetVisible(false);

    setScaling(m_scaling);

    GC_TRACE("Initialised UI");
}

UI::~UI()
{
    GC_TRACE("Destroying UI...");
    // Closes the documents and releases RmlUi's textures and geometry (through the render interface, which is still there)
    Rml::Shutdown();
}

Rml::Context& UI::getContext() { return *m_context; }

void UI::setScaling(const UIScaling& scaling)
{
    m_scaling = scaling;

    int width{}, height{};
    SDL_GetWindowSizeInPixels(m_window, &width, &height);
    const glm::ivec2 size(std::max(width, 1), std::max(height, 1));
    if (size != m_size) {
        m_size = size;
        m_context->SetDimensions(Rml::Vector2i(size.x, size.y));
    }

    float display_scale = SDL_GetWindowDisplayScale(m_window);
    if (!(display_scale > 0.0f)) {
        display_scale = 1.0f;
    }
    const float window_scale = (m_scaling.reference_height > 0.0f) ? static_cast<float>(size.y) / m_scaling.reference_height : display_scale;
    float ratio = 1.0f;
    switch (m_scaling.mode) {
    case UIScaleMode::AUTO:
        ratio = std::max(display_scale, window_scale);
        break;
    case UIScaleMode::DISPLAY:
        ratio = display_scale;
        break;
    case UIScaleMode::WINDOW_HEIGHT:
        ratio = window_scale;
        break;
    }
    ratio = std::max(ratio * m_scaling.user_scale, 0.1f);
    if (ratio != m_dp_ratio || m_context->GetDensityIndependentPixelRatio() != ratio) {
        m_dp_ratio = ratio;
        m_context->SetDensityIndependentPixelRatio(ratio);
    }
}

const UIScaling& UI::getScaling() const { return m_scaling; }

float UI::getDpRatio() const { return m_dp_ratio; }

glm::ivec2 UI::getSize() const { return m_size; }

Rml::ElementDocument* UI::loadDocument(Name name)
{
    // The file interface finds files by their names. In a release build a Name doesn't know its string, so this is "0x" and the hash
    return m_context->LoadDocument(name.getString());
}

bool UI::loadFontFace(Name name, bool fallback) { return Rml::LoadFontFace(name.getString(), fallback); }

void UI::reloadDocuments()
{
    GC_INFO("Reloading the UI's documents");
    m_file_interface->scanSourceDirectory();
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();
    Rml::ReleaseTextures(); // so that images are read again
    ++m_reload_count;
}

uint32_t UI::getReloadCount() const { return m_reload_count; }

void UI::processEvent(SDL_Event& ev, bool mouse_captured)
{
    // There is no cursor to use the UI with while the game has the mouse, so the game has the keyboard too
    if (mouse_captured) {
        return;
    }

    bool propagate = true; // RmlUi's functions return false when the UI used the event
    const int modifiers = getKeyModifiers();
    switch (ev.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        const float density = SDL_GetWindowPixelDensity(m_window);
        propagate = m_context->ProcessMouseMove(static_cast<int>(ev.motion.x * density), static_cast<int>(ev.motion.y * density), modifiers);
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        propagate = m_context->ProcessMouseButtonDown(convertMouseButton(ev.button.button), modifiers);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        propagate = m_context->ProcessMouseButtonUp(convertMouseButton(ev.button.button), modifiers);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        propagate = m_context->ProcessMouseWheel(-ev.wheel.y, modifiers);
        break;
    case SDL_EVENT_KEY_DOWN:
        propagate = m_context->ProcessKeyDown(convertKey(ev.key.key), modifiers);
        if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER) {
            propagate &= m_context->ProcessTextInput('\n');
        }
        break;
    case SDL_EVENT_KEY_UP:
        propagate = m_context->ProcessKeyUp(convertKey(ev.key.key), modifiers);
        break;
    case SDL_EVENT_TEXT_INPUT:
        propagate = m_context->ProcessTextInput(Rml::String(ev.text.text));
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        m_context->ProcessMouseLeave();
        break;
    default:
        break;
    }

    // (the engine's own keys always get through)
    const bool engine_key = (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) &&
                            (ev.key.scancode == SDL_SCANCODE_F10 || ev.key.scancode == SDL_SCANCODE_F11 || ev.key.scancode == SDL_SCANCODE_ESCAPE);
    if (!propagate && !engine_key && ev.type != SDL_EVENT_WINDOW_MOUSE_LEAVE) {
        ev.type = 0;
    }
}

void UI::update(UIDrawData& draw_data)
{
    ZoneScoped;

    setScaling(m_scaling); // (the window may have changed size, or moved to another display)
    m_context->Update();

    m_render_interface->beginFrame(draw_data, m_size);
    m_context->Render();
    m_render_interface->endFrame();
}

void UI::drawDebugWindow(bool* open)
{
    if (!open) {
        return;
    }
    if (ImGui::Begin("UI", open)) {
        ImGui::Text("%d x %d pixels, 1 dp = %.3f px", m_size.x, m_size.y, m_dp_ratio);
        ImGui::Text("Display scale %.2f", SDL_GetWindowDisplayScale(m_window));

        int mode = static_cast<int>(m_scaling.mode);
        UIScaling scaling = m_scaling;
        if (ImGui::Combo("Scale mode", &mode, "Auto\0Display\0Window height\0")) {
            scaling.mode = static_cast<UIScaleMode>(mode);
        }
        ImGui::DragFloat("Reference height", &scaling.reference_height, 10.0f, 100.0f, 4320.0f, "%.0f px");
        ImGui::SliderFloat("User scale", &scaling.user_scale, 0.5f, 2.0f, "x%.2f");
        if (scaling.mode != m_scaling.mode || scaling.reference_height != m_scaling.reference_height || scaling.user_scale != m_scaling.user_scale) {
            setScaling(scaling);
        }

        ImGui::Separator();
        if (ImGui::Button("Reload documents")) {
            reloadDocuments();
        }
        ImGui::SameLine();
        ImGui::TextDisabled(m_file_interface->hasSourceDirectory() ? "(from content/ui)" : "(from the packages)");
        if (ImGui::Checkbox("RmlUi debugger", &m_debugger_visible)) {
            Rml::Debugger::SetVisible(m_debugger_visible);
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Documents:");
        for (int i = 0; i < m_context->GetNumDocuments(); ++i) {
            const Rml::ElementDocument* const document = m_context->GetDocument(i);
            const std::string& url = document->GetSourceURL();
            if (url.find("rmlui-debug") != std::string::npos || url.empty()) {
                continue; // (the debugger's own)
            }
            ImGui::BulletText("%s%s", url.c_str(), document->IsVisible() ? "" : " (hidden)");
        }
    }
    ImGui::End();
}

} // namespace gc
