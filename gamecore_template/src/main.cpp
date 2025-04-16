#include <gamecore/gc_app.h>
#include <gamecore/gc_logger.h>
#include <gamecore/gc_jobs.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_abort.h>
#include <gamecore/gc_vulkan_common.h>
#include <gamecore/gc_vulkan_renderer.h>
#include <gamecore/gc_asset_id.h>
#include <gamecore/gc_content.h>
#include <gamecore/gc_vulkan_pipeline.h>
#include <gamecore/gc_vulkan_allocator.h>
#include <gamecore/gc_vulkan_swapchain.h>
#include <gamecore/gc_stopwatch.h>

#include <gtc/quaternion.hpp>
#include <mat4x4.hpp>
#include <glm.hpp>

#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>

#include <tracy/Tracy.hpp>

static std::pair<VkImage, VmaAllocation> createImage(VkDevice device, VmaAllocator allocator, VkFormat format, VkExtent2D extent, VkImage& image, VmaAllocation& allocation, VkImageView& view)
{
	VkImageCreateInfo image_info{};
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.pNext = nullptr;
	image_info.flags = 0;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = format;
	image_info.extent.width = extent.width;
	image_info.extent.height = extent.height;
	image_info.extent.depth = 1;
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.queueFamilyIndexCount = 0;     // ignored
	image_info.pQueueFamilyIndices = nullptr; // ingored
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VmaAllocationCreateInfo alloc_create_info{};
	alloc_create_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
	alloc_create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
	alloc_create_info.priority = 1.0f; 
	GC_CHECKVK(vmaCreateImage(allocator, &image_info, &alloc_create_info, &image, &allocation, nullptr));

	VkImageViewCreateInfo view_info{};
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = image_info.format;
	view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.baseArrayLayer = 0;
	view_info.subresourceRange.layerCount = 1;
	view_info.subresourceRange.baseMipLevel = 0;
	view_info.subresourceRange.levelCount = 1;
	GC_CHECKVK(vkCreateImageView(device, &view_info, nullptr, &view));

	return std::make_pair(image, allocation);
}

#if 0
static std::pair<VkImage, VmaAllocation> createImage(gc::VulkanRenderer& renderer, VkSemaphore timeline_semaphore, uint64_t value)
{

	const VkExtent2D extent = renderer.getSwapchain().getExtent();

	/* Create test image */
	VkImage image{};
	VmaAllocation image_allocation{};
	{
		ZoneScopedN("Create image on GPU");
		VkImageCreateInfo image_info{};
		image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image_info.pNext = nullptr;
		image_info.flags = 0;
		image_info.imageType = VK_IMAGE_TYPE_2D;
		image_info.format = renderer.getSwapchain().getSurfaceFormat().format;
		image_info.extent.width = extent.width;
		image_info.extent.height = extent.height;
		image_info.extent.depth = 1;
		image_info.mipLevels = 1;
		image_info.arrayLayers = 1;
		image_info.samples = VK_SAMPLE_COUNT_1_BIT;
		image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
		image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		image_info.queueFamilyIndexCount = 0;     // ignored
		image_info.pQueueFamilyIndices = nullptr; // ingored
		image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VmaAllocationCreateInfo alloc_create_info{};
		alloc_create_info.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
		alloc_create_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
		alloc_create_info.priority = 0.5f; 
		GC_CHECKVK(vmaCreateImage(renderer.getAllocator(), &image_info, &alloc_create_info, &image, &image_allocation, nullptr));
	}

	/* Create staging buffer */
	VkBuffer staging_buffer{};
	VmaAllocation staging_alloc{};
	{
		ZoneScopedN("Create staging buffer");
		VkBufferCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		info.flags = 0;
		info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		info.size = extent.width * extent.height * 4;
		VmaAllocationCreateInfo alloc_info{};
		alloc_info.usage = VMA_MEMORY_USAGE_AUTO;
		alloc_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
		alloc_info.priority = 0.5f;
		GC_CHECKVK(vmaCreateBuffer(renderer.getAllocator(), &info, &alloc_info, &staging_buffer, &staging_alloc, nullptr));
	}

	/* Fill staging buffer */
	{
		ZoneScopedN("Fill staging buffer");
		uint8_t* mapped = nullptr;
		GC_CHECKVK(vmaMapMemory(renderer.getAllocator(), staging_alloc, reinterpret_cast<void**>(&mapped)));
		/*
		for (size_t y = 0; y < extent.height; ++y) {
			for (size_t x = 0; x < extent.width; ++x) {
				int r = rand();
				mapped[(y * extent.width + x) * 4 + 1] = static_cast<uint8_t>(r);
				mapped[(y * extent.width + x) * 4 + 2] = static_cast<uint8_t>(r >> 8);
				mapped[(y * extent.width + x) * 4 + 3] = static_cast<uint8_t>(r >> 16);
				mapped[(y * extent.width + x) * 4 + 4] = static_cast<uint8_t>(255);
			}
		}
		*/
		memset(mapped, 255u, extent.width * extent.height * 4);
		vmaUnmapMemory(renderer.getAllocator(), staging_alloc);
	}

	/* Create command pool */
	VkCommandPool pool{};
	{
		ZoneScopedN("Create command pool");
		VkCommandPoolCreateInfo pool_info{};
		pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		pool_info.queueFamilyIndex = renderer.getDevice().getMainQueueFamilyIndex();
		GC_CHECKVK(vkCreateCommandPool(renderer.getDevice().getHandle(), &pool_info, nullptr, &pool));
	}

	/* Allocate command buffer */
	VkCommandBuffer copybuf{};
	{
		ZoneScopedN("Allocate command buffer");
		VkCommandBufferAllocateInfo cmd_info{};
		cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cmd_info.commandPool = pool;
		cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cmd_info.commandBufferCount = 1;
		GC_CHECKVK(vkAllocateCommandBuffers(renderer.getDevice().getHandle(), &cmd_info, &copybuf));
	}

	/* Create fence */
	VkFence fence{};
	if (finished_semaphore == VK_NULL_HANDLE) {
		VkFenceCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		GC_CHECKVK(vkCreateFence(renderer.getDevice().getHandle(), &info, nullptr, &fence));
	}

	{
		ZoneScopedN("Record command buffer");
		/* Record command buffer */
		VkCommandBufferBeginInfo begin_info{};
		begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		GC_CHECKVK(vkBeginCommandBuffer(copybuf, &begin_info));

		/* Transition acquired swapchain image to TRANSFER_DST layout */
		{
			VkImageMemoryBarrier2 barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
			barrier.srcAccessMask = VK_ACCESS_2_NONE;
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image;
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.subresourceRange.baseMipLevel = 0;
			barrier.subresourceRange.levelCount = 1;
			barrier.subresourceRange.baseArrayLayer = 0;
			barrier.subresourceRange.layerCount = 1;
			VkDependencyInfo dep{};
			dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			dep.imageMemoryBarrierCount = 1;
			dep.pImageMemoryBarriers = &barrier;
			vkCmdPipelineBarrier2(copybuf, &dep);
		}

		{ /* Do the copy */
			VkBufferImageCopy copy_region{};
			copy_region.bufferOffset = 0;
			copy_region.bufferRowLength = 0;
			copy_region.bufferImageHeight = 0;
			copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copy_region.imageSubresource.layerCount = 1;
			copy_region.imageSubresource.baseArrayLayer = 0;
			copy_region.imageSubresource.mipLevel = 0;
			copy_region.imageExtent.width = extent.width;
			copy_region.imageExtent.height = extent.height;
			copy_region.imageExtent.depth = 1;
			vkCmdCopyBufferToImage(copybuf, staging_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);
		}

		/* Transition image to TRANSFER_SRC layout */
		{ 	
			VkImageMemoryBarrier2 barrier{};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image;
			barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barrier.subresourceRange.baseMipLevel = 0;
			barrier.subresourceRange.levelCount = 1;
			barrier.subresourceRange.baseArrayLayer = 0;
			barrier.subresourceRange.layerCount = 1;
			VkDependencyInfo dep{};
			dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			dep.imageMemoryBarrierCount = 1;
			dep.pImageMemoryBarriers = &barrier;
			vkCmdPipelineBarrier2(copybuf, &dep);
		}

		/* End command buffer */
		{
			GC_CHECKVK(vkEndCommandBuffer(copybuf));
		}
	}

	/* Submit command buffer */
	{
		ZoneScopedN("Submit command buffer");
		VkSubmitInfo submit{};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &copybuf;
		submit.waitSemaphoreCount = (finished_semaphore != VK_NULL_HANDLE) ? 1 : 0;
		submit.pWaitSemaphores = (finished_semaphore != VK_NULL_HANDLE) ? &finished_semaphore : nullptr;
		GC_CHECKVK(vkQueueSubmit(renderer.getDevice().getMainQueue(), 1, &submit, fence));
	}

	if (fence != VK_NULL_HANDLE) {
		ZoneScopedN("Wait for copy fence");
		GC_CHECKVK(vkWaitForFences(renderer.getDevice().getHandle(), 1, &fence, VK_FALSE, UINT64_MAX));
		vkDestroyFence(renderer.getDevice().getHandle(), fence, nullptr);
	}

	vkDestroyCommandPool(renderer.getDevice().getHandle(), pool, nullptr);
	vmaDestroyBuffer(renderer.getAllocator(), staging_buffer, staging_alloc);

	return std::make_pair(image, image_allocation);
}
#endif

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    gc::App::initialise();

    gc::Window& win = gc::app().window();
    gc::VulkanRenderer& renderer = gc::app().vulkanRenderer();

    win.setTitle("Hello world!");
    win.setIsResizable(true);
    win.setWindowVisibility(true);

	VkSemaphore timeline_semaphore{};
	{
		VkSemaphoreTypeCreateInfo type_info{};
		type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
		type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		type_info.initialValue = 0;
		VkSemaphoreCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		info.pNext = &type_info;
		GC_CHECKVK(vkCreateSemaphore(renderer.getDevice().getHandle(), &info, nullptr, &timeline_semaphore));
	}

	struct FIFStuff {
		VkImage image;
		VmaAllocation allocation;
		VkImageView view;
		VkCommandPool pool;
		VkCommandBuffer cmd;
		uint64_t render_finished_value;
		uint64_t present_finished_value;
	};

	std::array<FIFStuff, 2> fif{};
	for (auto& stuff : fif) {
		createImage(renderer.getDevice().getHandle(), renderer.getAllocator(), renderer.getSwapchain().getSurfaceFormat().format, renderer.getSwapchain().getExtent(), stuff.image, stuff.allocation, stuff.view);

		{
			VkCommandPoolCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
			info.queueFamilyIndex = renderer.getDevice().getMainQueueFamilyIndex();
			GC_CHECKVK(vkCreateCommandPool(renderer.getDevice().getHandle(), &info, nullptr, &stuff.pool));
		}

		{
			VkCommandBufferAllocateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			info.commandBufferCount = 1;
			info.commandPool = stuff.pool;
			info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			GC_CHECKVK(vkAllocateCommandBuffers(renderer.getDevice().getHandle(), &info, &stuff.cmd));
		}

		stuff.render_finished_value = 0;
		stuff.present_finished_value = 0;

	}

	uint64_t frame_count = 0;
	uint64_t timeline_value = 0;

    while (!win.shouldQuit()) {

        win.processEvents();

        {
            ZoneScopedN("UI Logic");
            if (win.getKeyDown(SDL_SCANCODE_ESCAPE)) {
                win.setQuitFlag();
            }
            if (win.getKeyPress(SDL_SCANCODE_F11)) {
                if (win.getIsFullscreen()) {
                    win.setSize(0, 0, false);
                }
                else {
                    win.setSize(0, 0, true);
                }
            }
            if (win.getButtonPress(gc::MouseButton::X1)) {
                // show/hide mouse
                if (!SDL_SetWindowRelativeMouseMode(win.getHandle(), !SDL_GetWindowRelativeMouseMode(win.getHandle()))) {
                    GC_ERROR("SDL_SetWindowRelativeMouseMode() error: {}", SDL_GetError());
                }
            }
        }

		auto& stuff = fif[frame_count % fif.size()];

		{
			ZoneScopedN("Wait for semaphore");
			VkSemaphoreWaitInfo info{};
			info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
			info.semaphoreCount = 1;
			info.pSemaphores = &timeline_semaphore;
			info.pValues = &stuff.render_finished_value;
			GC_CHECKVK(vkWaitSemaphores(renderer.getDevice().getHandle(), &info, UINT64_MAX));
		}

		{
			ZoneScopedN("Reset command pool");
			// do rendering
			GC_CHECKVK(vkResetCommandPool(renderer.getDevice().getHandle(), stuff.pool, 0));
		}

		{
			ZoneScopedN("Record cmd buf");
			{
				VkCommandBufferBeginInfo info{};
				info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
				info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
				GC_CHECKVK(vkBeginCommandBuffer(stuff.cmd, &info));
			}

			/* Transition image to COLOR_ATTACHMENT_OPTIMAL layout */
			{
				VkImageMemoryBarrier2 barrier{};
				barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
				barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
				barrier.srcAccessMask = VK_ACCESS_2_NONE;
				barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
				barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
				barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.image = stuff.image;
				barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				barrier.subresourceRange.baseMipLevel = 0;
				barrier.subresourceRange.levelCount = 1;
				barrier.subresourceRange.baseArrayLayer = 0;
				barrier.subresourceRange.layerCount = 1;
				VkDependencyInfo dep{};
				dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
				dep.imageMemoryBarrierCount = 1;
				dep.pImageMemoryBarriers = &barrier;
				vkCmdPipelineBarrier2(stuff.cmd, &dep);
			}

			{
				VkRenderingAttachmentInfo att{};
				att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
				att.imageView = stuff.view;
				att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				att.resolveMode = VK_RESOLVE_MODE_NONE;
				att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
				att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
				att.clearValue.color.float32[0] = (sinf(static_cast<float>(frame_count) * 0.8e-2f) + 1.0f) * 0.5f;
				att.clearValue.color.float32[1] = (cosf(static_cast<float>(frame_count) * 0.2e-2f) + 1.0f) * 0.5f;
				att.clearValue.color.float32[2] = (sinf(static_cast<float>(frame_count) * 1.0e-2f) + 1.0f) * 0.5f;
				att.clearValue.color.float32[3] = 1.0f;
				VkRenderingInfo info{};
				info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
				info.renderArea.offset.x = 0;
				info.renderArea.offset.y = 0;
				info.renderArea.extent = renderer.getSwapchain().getExtent();
				info.layerCount = 1;
				info.viewMask = 0;
				info.colorAttachmentCount = 1;
				info.pColorAttachments = &att;
				info.pDepthAttachment = nullptr;
				info.pStencilAttachment = nullptr;
				vkCmdBeginRendering(stuff.cmd, &info);
			}

			{
				vkCmdEndRendering(stuff.cmd);
			}

			/* Transition image to TRANSFER_SRC layout */
			{ 	
				VkImageMemoryBarrier2 barrier{};
				barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
				barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
				barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
				barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
				barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
				barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.image = stuff.image;
				barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				barrier.subresourceRange.baseMipLevel = 0;
				barrier.subresourceRange.levelCount = 1;
				barrier.subresourceRange.baseArrayLayer = 0;
				barrier.subresourceRange.layerCount = 1;
				VkDependencyInfo dep{};
				dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
				dep.imageMemoryBarrierCount = 1;
				dep.pImageMemoryBarriers = &barrier;
				vkCmdPipelineBarrier2(stuff.cmd, &dep);
			}

			GC_CHECKVK(vkEndCommandBuffer(stuff.cmd));
		}

		/* Submit command buffer */
		{
			ZoneScopedN("Submit command buffer");

			VkCommandBufferSubmitInfo cmd_info{};
			cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
			cmd_info.commandBuffer = stuff.cmd;

			VkSemaphoreSubmitInfo wait_info{};
			wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
			wait_info.semaphore = timeline_semaphore;
			wait_info.stageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
			wait_info.value = stuff.present_finished_value;

			timeline_value += 1;
			stuff.render_finished_value = timeline_value;

			VkSemaphoreSubmitInfo signal_info{};
			signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
			signal_info.semaphore = timeline_semaphore;
			signal_info.stageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
			signal_info.value = timeline_value;

			VkSubmitInfo2 submit{};
			submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
			submit.waitSemaphoreInfoCount = 1;
			submit.pWaitSemaphoreInfos = &wait_info;
			submit.commandBufferInfoCount = 1;
			submit.pCommandBufferInfos = &cmd_info;
			submit.signalSemaphoreInfoCount = 1;
			submit.pSignalSemaphoreInfos = &signal_info;
			GC_CHECKVK(vkQueueSubmit2(renderer.getDevice().getMainQueue(), 1, &submit, VK_NULL_HANDLE));
		}

		// Queue image for presentation
        const bool resized = renderer.getSwapchain().acquireAndPresent(stuff.image, win.getResizedFlag(), timeline_semaphore, timeline_value);
		timeline_value += 1;

		stuff.present_finished_value = timeline_value; // Timeline semaphore will reach this value when image can be used again.

		// Handle resize
		if (resized) {
			for (auto& stuff : fif) {
				vkDestroyImageView(renderer.getDevice().getHandle(), stuff.view, nullptr);
				vmaDestroyImage(renderer.getAllocator(), stuff.image, stuff.allocation);
				createImage(renderer.getDevice().getHandle(), renderer.getAllocator(), renderer.getSwapchain().getSurfaceFormat().format, renderer.getSwapchain().getExtent(), stuff.image, stuff.allocation, stuff.view);
			}
		}

		++frame_count;
        FrameMark;
    }

    renderer.waitIdle();

	for (const auto& stuff : fif) {
		vkDestroyCommandPool(renderer.getDevice().getHandle(), stuff.pool, nullptr);
		vkDestroyImageView(renderer.getDevice().getHandle(), stuff.view, nullptr);
		vmaDestroyImage(renderer.getAllocator(), stuff.image, stuff.allocation);
	}

	vkDestroySemaphore(renderer.getDevice().getHandle(), timeline_semaphore, nullptr);

    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame() therefore main() can always return 0
    return 0;
}
