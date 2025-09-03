#include "gamecore/gc_debug_ui.h"

#include <filesystem>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>

#include <SDL3/SDL_video.h>
#include <SDL3/SDL_filesystem.h>

#include "gamecore/gc_logger.h"
#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_render_backend.h"

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
        info.RenderPass = VK_NULL_HANDLE;

        // There is no reason why the ImGui Vulkan backend should need to know about the swapchain image count.
        // Using 2 works fine here.
        info.MinImageCount = 2;
        info.ImageCount = info.MinImageCount;

        info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.UseDynamicRendering = true;
        info.PipelineRenderingCreateInfo = rendering_info;
        if (!ImGui_ImplVulkan_Init(&info)) {
            gc::abortGame("ImGui_ImplVulkan_Init() error");
        }
    }

    this->active = true;

    GC_TRACE("Initialised DebugUI");
}

DebugUI::~DebugUI()
{
    GC_TRACE("Destroying DebugUI...");
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(m_imgui_ctx);
}

void DebugUI::update()
{
    ImGui_ImplSDL3_NewFrame();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();

    if (this->active) {
        ImGui::Begin("Debug UI", &this->active);
        ImGui::Checkbox("Show ImGui Demo", &m_show_demo);
        ImGui::End();

        if (m_show_demo) {
            ImGui::ShowDemoWindow(&m_show_demo);
        }
    }

    ImGui::Render();
}

} // namespace gc