#include "gamecore/gc_debug_ui.h"

#include <cmath>

#include <filesystem>
#include <format>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>

#include <SDL3/SDL_video.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_timer.h>

#include <tracy/Tracy.hpp>

#include "gclog/gclog.h"
#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_render_backend.h"
#include "gamecore/gc_frame_state.h"
#include "gamecore/gc_content.h"
#include "gamecore/gc_units.h"

namespace gc {

DebugUI::DebugUI(SDL_Window* window, const RenderBackendInfo& render_backend_info, const std::filesystem::path& config_file)
{
    m_imgui_ctx = ImGui::CreateContext();

    m_config_file = config_file.string();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = m_config_file.c_str();

    ImGui_ImplSDL3_InitForVulkan(window);

    /* Load Vulkan functions for ImGui backend */
    {
        auto loader_func = [](const char* function_name, void* user_data) -> PFN_vkVoidFunction {
            return vkGetInstanceProcAddr(*reinterpret_cast<VkInstance*>(user_data), function_name);
        };
        VkInstance instance = render_backend_info.instance;
        if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, loader_func, &instance)) {
            gc::abortGame("ImGui_ImplVulkan_LoadFunctions() error");
        }
    }

    /* Init ImGui Vulkan Backend */
    {
        VkPipelineRenderingCreateInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering_info.pNext = nullptr;
        rendering_info.viewMask = 0;
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachmentFormats = &render_backend_info.framebuffer_format;
        rendering_info.depthAttachmentFormat = render_backend_info.depth_stencil_format;
        rendering_info.stencilAttachmentFormat = render_backend_info.depth_stencil_format;
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_3;
        info.Instance = render_backend_info.instance;
        info.PhysicalDevice = render_backend_info.physical_device;
        info.Device = render_backend_info.device;
        info.QueueFamily = render_backend_info.main_queue_family_index;
        info.Queue = render_backend_info.main_queue;
        info.DescriptorPool = render_backend_info.main_descriptor_pool;

        // There is no reason why the ImGui Vulkan backend should need to know about the swapchain image count.
        // Using 2 works fine here.
        info.MinImageCount = 2;
        info.ImageCount = info.MinImageCount;

        info.UseDynamicRendering = true;
        info.PipelineInfoMain.MSAASamples = render_backend_info.msaa_samples;
        info.PipelineInfoMain.PipelineRenderingCreateInfo = rendering_info;

        // info.MinAllocationSize = 1024 * 1024; // stop 'best practices' complaining

        if (!ImGui_ImplVulkan_Init(&info)) {
            gc::abortGame("ImGui_ImplVulkan_Init() error");
        }
    }

    this->active = false;

    GC_TRACE("Initialised DebugUI");
}

DebugUI::~DebugUI()
{
    GC_TRACE("Destroying DebugUI...");
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(m_imgui_ctx);
}

void DebugUI::newFrame()
{
    ZoneScoped;
    ImGui_ImplSDL3_NewFrame();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
}

void DebugUI::render()
{
    ZoneScoped;
    ImGui::Render();
}

void DebugUI::update(FrameState& frame_state)
{
    ZoneScoped;

    if (this->active) {
        // Everything is reached from one bar along the top, rather than each thing having a window that is always open
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("Windows")) {
                for (DebugWindow& window : m_windows) {
                    ImGui::MenuItem(window.name.c_str(), nullptr, &window.open);
                }
                ImGui::MenuItem("ImGui Demo", nullptr, &m_show_demo);
                ImGui::Separator();
                if (ImGui::MenuItem("Close All")) {
                    for (DebugWindow& window : m_windows) {
                        window.open = false;
                    }
                    m_show_demo = false;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Render")) {
                ImGui::MenuItem("Disable world rendering", nullptr, &m_clear_draw_data);
                ImGui::Separator();
                // these change what the world asked for, see below
                ImGui::MenuItem("Ambient light", nullptr, &m_ambient_light);
                ImGui::BeginDisabled(!m_ambient_light);
                ImGui::SetNextItemWidth(160.0f);
                ImGui::SliderFloat("Ambient scale", &m_ambient_light_scale, 0.0f, 4.0f, "x%.2f");
                ImGui::EndDisabled();
                ImGui::MenuItem("Shadows", nullptr, &m_shadows);
                ImGui::SetNextItemWidth(160.0f);
                ImGui::SliderFloat("Exposure", &m_exposure_compensation, -8.0f, 8.0f, "%+.1f EV");
                if (ImGui::MenuItem("Reset")) {
                    m_ambient_light = true;
                    m_ambient_light_scale = 1.0f;
                    m_shadows = true;
                    m_exposure_compensation = 0.0f;
                }
                ImGui::EndMenu();
            }

            // on the right hand side
            const std::string stats = std::format("{:.2f} ms ({} fps)   F10: hide", frame_state.average_frame_time * 1000.0,
                                                  static_cast<int>(std::round(1.0 / frame_state.average_frame_time)));
            const float stats_x = ImGui::GetWindowWidth() - ImGui::CalcTextSize(stats.c_str()).x - ImGui::GetStyle().ItemSpacing.x;
            if (stats_x > ImGui::GetCursorPosX()) {
                ImGui::SetCursorPosX(stats_x);
            }
            ImGui::TextUnformatted(stats.c_str());

            ImGui::EndMainMenuBar();
        }

        if (m_show_demo) {
            ImGui::ShowDemoWindow(&m_show_demo);
        }
    }

    if (m_clear_draw_data) {
        frame_state.draw_data.reset();
    }

    // The Render menu's settings stay in force while the debug UI is hidden
    frame_state.draw_data.setAmbientLight(m_ambient_light ? frame_state.draw_data.getAmbientLight() * m_ambient_light_scale : glm::vec3{0.0f, 0.0f, 0.0f});
    if (!m_shadows) {
        frame_state.draw_data.setShadowMap(nullptr);
    }
    frame_state.draw_data.setExposure(frame_state.draw_data.getExposure() * std::exp2(m_exposure_compensation));
}

bool* DebugUI::getWindowOpen(const char* name)
{
    DebugWindow* window = nullptr;
    for (DebugWindow& w : m_windows) {
        if (w.name == name) {
            window = &w;
            break;
        }
    }
    if (!window) {
        window = &m_windows.emplace_back(name, false);
    }
    return (this->active && window->open) ? &window->open : nullptr;
}

void DebugUI::windowEventInterceptor(SDL_Event& ev, bool mouse_captured)
{
    if (mouse_captured) {
        return;
    }

    ImGui_ImplSDL3_ProcessEvent(&ev);

    // cancel inputs that ImGui wants to intercept by setting ev.type to zero
    const ImGuiIO& io = ImGui::GetIO();
    // (F10 always gets through, so that the debug UI can be hidden while typing in one of its text boxes)
    if (io.WantCaptureKeyboard && (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) && ev.key.scancode != SDL_SCANCODE_F10) {
        ev.type = 0;
    }
    if (io.WantCaptureMouse && (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP || ev.type == SDL_EVENT_MOUSE_MOTION ||
                                ev.type == SDL_EVENT_MOUSE_WHEEL)) {
        ev.type = 0;
    }
}

bool DebugUI::postRenderCallback(VkCommandBuffer cmd)
{
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (!draw_data) {
        return false;
    }
    ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
    return true;
}

} // namespace gc
