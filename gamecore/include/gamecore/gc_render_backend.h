/*
 * This is the engine's rendering backend.
 * It operates at a relatively high level while directly calling the Vulkan API.
 * This is done instead of creating a Vulkan abstraction which would end up effectively being an OpenGL remake.
 * The kind of things this renderer should do include:
 *  - Managing render targets
 *  - Presenting to the screen
 *  - ImGui integration
 *  - Drawing UI
 *  - Drawing 3D meshes with materials/textures
 *  - Applying post-processing effects such as FXAA and bloom
 * Things the renderer should not do include:
 *  - Frustrum culling
 *  - GPU resource streaming (though this class should contain methods to upload/free GPU resources)
 *  - Anything that would involve accessing/modifying scene data (It should have no knowledge of what a 'scene' is)
 * For example, to render the 3D world, the application would give RenderBackend a list of GPU mesh handles, textures, etc to draw.
 * The RenderBackend should not be aware of things like LOD selection, and should assume all draw call data given to it is valid and all resources are resident
 * in GPU memory.
 */

#pragma once

#include <array>
#include <memory>
#include <span>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <gctemplates/gct_static_vector.h>

#include "gamecore/gc_vulkan_common.h"
#include "gamecore/gc_vulkan_device.h"
#include "gamecore/gc_vulkan_allocator.h"
#include "gamecore/gc_vulkan_swapchain.h"
#include "gamecore/gc_gpu_resources.h"
#include "gamecore/gc_render_texture.h"
#include "gamecore/gc_render_mesh.h"
#include "gamecore/gc_mesh_vertex.h"
#include "gamecore/gc_render_material.h"
#include "gamecore/gc_render_buffer.h"
#include "gamecore/gc_render_shadow_map.h"

struct SDL_Window; // forward-dec

namespace gc {

class WorldDrawData; // forward-dec

// Handles and settings needed for setting up ImGui's Vulkan backend
struct RenderBackendInfo {
    VkInstance instance;
    VkDevice device;
    VkPhysicalDevice physical_device;
    VkQueue main_queue;
    uint32_t main_queue_family_index;
    VkDescriptorPool main_descriptor_pool;
    VkFormat framebuffer_format;
    VkFormat depth_stencil_format;
    VkSampleCountFlagBits msaa_samples;
};

enum class RenderSyncMode { VSYNC_ON_DOUBLE_BUFFERED, VSYNC_ON_TRIPLE_BUFFERED, VSYNC_ON_TRIPLE_BUFFERED_UNTHROTTLED, VSYNC_OFF };

class RenderBackend {
    VulkanDevice m_device;
    VulkanAllocator m_allocator;
    VulkanSwapchain m_swapchain;

    GPUResourceDeleteQueue m_delete_queue;

    // global descriptor pool
    VkSampler m_sampler{};
    VkDescriptorPool m_main_descriptor_pool{};
    VkDescriptorSetLayout m_frame_set_layout{};
    VkDescriptorSetLayout m_material_set_layout{};
    VkDescriptorSetLayout m_shadow_set_layout{};
    VkSampler m_shadow_sampler{};

    // pipeline layout for most 3D rendering
    VkPipelineLayout m_world_pipeline_layout{};

    // The shaders that draw the world, see setWorldShaders()
    VkShaderModule m_single_draw_vertex_module{};
    VkShaderModule m_instanced_vertex_module{};
    VkShaderModule m_fragment_module{};

    // Pipeline variants, indexed by getMaterialPipelineVariant(). One is made the first time a material that needs it is drawn.
    std::array<std::unique_ptr<GPUPipeline>, MATERIAL_PIPELINE_VARIANTS> m_main_pipelines{};
    std::array<std::unique_ptr<GPUPipeline>, MATERIAL_PIPELINE_VARIANTS> m_instancing_pipelines{};

    VkSampleCountFlagBits m_msaa_samples{};

    uint64_t m_frame_count{};

    // Skybox stuff
    // VkPipeline m_skybox_pipeline{};

    // Images:

    VkImage m_color_attachment_image{};
    VmaAllocation m_color_attachment_allocation{};
    VkImageView m_color_attachment_image_view{};

    VkFormat m_depth_stencil_attachment_format{};
    VkImage m_depth_stencil_attachment_image{};
    VmaAllocation m_depth_stencil_attachment_allocation{};
    VkImageView m_depth_stencil_attachment_view{};

    VkImage m_framebuffer_image{};
    VmaAllocation m_framebuffer_image_allocation{};
    VkImageView m_framebuffer_image_view{};

    /* Important synchronisation objects */
    /* If the number of frames-in-flight changes, everything here is reset */
    struct FIFStuff {
        VkCommandPool pool;
        VkCommandBuffer cmd;
        uint64_t command_buffer_available_value;
    };
    gct::static_vector<FIFStuff, 2> m_fif{};
    int m_requested_frames_in_flight{};
    VkSemaphore m_main_timeline_semaphore{};
    uint64_t m_main_timeline_value{};
    uint64_t m_framebuffer_copy_finished_value{}; // there;s only one framebuffer so it's not in FIFStuff

    VkCommandPool m_transfer_cmd_pool{};
    VkSemaphore m_transfer_timeline_semaphore{};
    uint64_t m_transfer_timeline_value{};
    
    std::unique_ptr<RenderBuffer> m_frame_uniform_buffer{};
    std::unique_ptr<RenderBuffer> m_instancing_transforms_buffer{};
    
    std::unique_ptr<GPUDescriptorSet> m_frame_uniform_buffer_set{}; // permanently points to m_frame_uniform_buffer

    // A single texel that shadows nothing. Bound when the world has no shadow map, as the shaders always read one.
    std::unique_ptr<RenderShadowMap> m_no_shadow_map{};

#ifdef TRACY_ENABLE
    struct TracyVulkanContext {
        VkCommandPool pool;
        VkCommandBuffer cmd;
        TracyVkCtx ctx;
    };
    TracyVulkanContext m_tracy_vulkan_context{};
#endif

public:
    explicit RenderBackend(SDL_Window* window_handle);
    RenderBackend(const RenderBackend&) = delete;

    ~RenderBackend();

    RenderBackend operator=(const RenderBackend&) = delete;

    // configure renderer
    void setSyncMode(RenderSyncMode mode);

    /* Renders to framebuffer and presents framebuffer to the screen */
    void submitFrame(bool window_resized, const WorldDrawData& world_draw_data, bool (*postRenderCallback)(VkCommandBuffer cmd) = nullptr);

    /* Destroys any GPU resources that have been added to the delete queue and are not in use */
    void cleanupGPUResources();

    // Sets the shaders that the world is drawn with: a vertex shader for single draws (world matrix in a push constant), one for
    // instanced draws (world matrix in vertex attributes) and the fragment shader they share. Call once, before the first frame.
    // The pipelines are made from these later, one for each combination of MaterialFeatures that gets drawn. The fragment shader
    // is told which features it has through specialization constants 0, 1, 2... (one bool per feature, in the order they are
    // declared), so that it only samples the textures the material has, and the blend mode through the next one (an int, the value
    // of the MaterialBlendMode). A shader without those constants is the same in every pipeline.
    void setWorldShaders(std::span<const uint8_t> single_draw_vertex_spv, std::span<const uint8_t> instanced_vertex_spv,
                         std::span<const uint8_t> fragment_spv);

    RenderTexture createTexture(std::span<const uint8_t> r8g8b8a8_pak, bool srgb);
    RenderTexture createCubeTexture(std::array<std::span<const uint8_t>, 6> r8g8b8a8_paks, bool srgb);
    // r16_pak is the data of a SHADOW_MAP_R16 asset
    RenderShadowMap createShadowMap(std::span<const uint8_t> r16_pak);
    RenderMesh createMesh(std::span<const MeshVertex> vertices, std::span<const uint16_t> indices);
    // A texture that isn't in 'features' is never sampled, but one still has to be given (any will do).
    RenderMaterial createMaterial(RenderTexture& base_color, RenderTexture& orm, RenderTexture& normal, RenderTexture& emissive, MaterialFeatures features,
                                  MaterialBlendMode blend_mode, const MaterialConstants& constants);

    RenderBackendInfo getInfo() const
    {
        RenderBackendInfo info{};
        info.instance = m_device.getInstance();
        info.device = m_device.getHandle();
        info.physical_device = m_device.getPhysicalDevice();
        info.main_queue = m_device.getMainQueue();
        info.main_queue_family_index = m_device.getQueueFamilyIndex();
        info.main_descriptor_pool = m_main_descriptor_pool;
        info.framebuffer_format = m_swapchain.getSurfaceFormat().format;
        info.depth_stencil_format = m_depth_stencil_attachment_format;
        info.msaa_samples = m_msaa_samples;
        return info;
    }

    VkDevice getDevice() const { return m_device.getHandle(); }

    void waitIdle(); // waits for all Vulkan queues to finish

private:
    // pak is a width, a height, and then the texels
    RenderTexture createTextureFromPak(std::span<const uint8_t> pak, VkFormat image_format, uint32_t bytes_per_texel, bool generate_mips);

    GPUPipeline createPipeline(VkShaderModule vertex_module, VkShaderModule fragment_module, uint32_t variant,
                               const VkPipelineVertexInputStateCreateInfo& vertex_input_state);
    // variant is from getMaterialPipelineVariant()
    GPUPipeline createMainPipeline(uint32_t variant);
    GPUPipeline createInstancingPipeline(uint32_t variant);

    // Makes the pipeline variants that are needed to draw this, if they don't exist yet
    void createMissingPipelines(const WorldDrawData& world_draw_data);

    void recreateFramesInFlightResources();

    // Call this when the swapchain is resized
    void recreateRenderImages();

    // Call this before input polling and logic to reduce latency at the cost of stalling GPU
    void waitForFrameReady();
};

} // namespace gc
