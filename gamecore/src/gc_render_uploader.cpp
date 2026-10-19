#include "gamecore/gc_render_uploader.h"

#include <cstring>

#include <algorithm>
#include <array>
#include <tuple>
#include <vector>

#include <tracy/Tracy.hpp>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_threading.h"
#include "gamecore/gc_units.h"
#include "gamecore/gc_vulkan_utils.h"
#include "gclog/gclog.h"

namespace gc {

// The original image (mip level 0) should be TRANSFER_SRC image layout, the rest should be TRANSFER_DST
// At the end, will transition all mip levels to SHADER_READ_ONLY_OPTIMAL
static void generateMipMaps(VkCommandBuffer cmd, VkImage image, uint32_t width, uint32_t height)
{
    VkImageBlit region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.baseArrayLayer = 0;
    region.srcSubresource.layerCount = 1;
    region.srcOffsets[0] = {0, 0, 0};
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.baseArrayLayer = 0;
    region.dstSubresource.layerCount = 1;
    region.dstOffsets[1] = {0, 0, 0};

    std::array<VkImageMemoryBarrier2, 2> barriers{};
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    barriers[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = image;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.baseArrayLayer = 0;
    barriers[0].subresourceRange.layerCount = 1;
    barriers[1] = barriers[0];
    barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    barriers[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dep.pImageMemoryBarriers = barriers.data();

    uint32_t level_width = width;
    uint32_t level_height = height;
    uint32_t mip_dst_index = 0;
    while (level_width > 1 || level_height > 1) {
        const uint32_t src_width = level_width;
        const uint32_t src_height = level_height;
        level_width = (level_width > 1) ? level_width / 2 : 1;
        level_height = (level_height > 1) ? level_height / 2 : 1;
        ++mip_dst_index;
        const uint32_t mip_src_index = mip_dst_index - 1;

        // blit mip_src_index to mip_dst_index
        region.srcSubresource.mipLevel = mip_src_index;
        region.srcOffsets[1] = {static_cast<int>(src_width), static_cast<int>(src_height), 1};
        region.dstSubresource.mipLevel = mip_dst_index;
        region.dstOffsets[1] = {static_cast<int>(level_width), static_cast<int>(level_height), 1};
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);

        // barrier for mip_src_index TRANSFER_SRC_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
        barriers[0].subresourceRange.baseMipLevel = mip_src_index;
        // barrier for mip_dst_index TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL
        barriers[1].subresourceRange.baseMipLevel = mip_dst_index;
        if (level_width == 1 && level_height == 1) {
            // last mip level won't be blitted from so make it SHADER_READ_ONLY_OPTIMAL
            barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barriers[1].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        vkCmdPipelineBarrier2(cmd, &dep);
    }
}

void RenderUpload::waitForSubmit()
{
    if (m_state && !m_state->submitted) {
        m_state->uploader->waitForSubmit(*m_state);
    }
}

GPUTexture RenderUpload::takeTexture()
{
    GC_ASSERT(m_state && m_state->submitted && m_state->image);
    GPUTexture texture(m_state->uploader->getDeleteQueue(), m_state->image, m_state->image_allocation, m_state->image_view);
    texture.useResource(m_state->timeline_semaphore, m_state->signal_value);
    m_state.reset();
    return texture;
}

RenderUploader::RenderUploader(VkDevice device, VmaAllocator allocator, GPUResourceDeleteQueue& delete_queue, VkQueue transfer_queue,
                               uint32_t queue_family_index, VkSemaphore timeline_semaphore)
    : m_device(device),
      m_allocator(allocator),
      m_delete_queue(delete_queue),
      m_transfer_queue(transfer_queue),
      m_queue_family_index(queue_family_index),
      m_timeline_semaphore(timeline_semaphore)
{
    VkCommandPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.queueFamilyIndex = m_queue_family_index;
    info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    GC_CHECKVK(vkCreateCommandPool(m_device, &info, nullptr, &m_cmd_pool));

    m_thread = std::thread([this]() { runWorkThread(); });
}

RenderUploader::~RenderUploader()
{
    {
        const std::lock_guard lock(m_mutex);
        m_shutdown = true;
    }
    m_work_available.notify_all();
    m_thread.join();

    for (const Upload& upload : m_finished) {
        destroyUnsubmitted(upload);
    }
    if (!m_finished.empty() || !m_queued.empty()) {
        GC_DEBUG("{} uploads were still waiting for the render work thread at shutdown", m_finished.size() + m_queued.size());
    }

    vkDestroyCommandPool(m_device, m_cmd_pool, nullptr);
}

RenderUpload RenderUploader::uploadImage(VkFormat format, uint32_t width, uint32_t height, uint32_t mip_levels, bool cube_map, const Sources& sources,
                                         RenderUploadMode mode)
{
    GC_ASSERT(format != VK_FORMAT_UNDEFINED);
    GC_ASSERT(width > 0 && height > 0 && mip_levels >= 1);
    GC_ASSERT(mip_levels == 1 || !cube_map); // mipmaps are only generated for the first layer

    Upload upload{};
    upload.sources = sources;
    upload.format = format;
    upload.width = width;
    upload.height = height;
    upload.mip_levels = mip_levels;
    upload.cube_map = cube_map;
    return startUpload(std::move(upload), mode);
}

RenderUpload RenderUploader::uploadImageLevels(VkFormat format, uint32_t texel_size, uint32_t width, uint32_t height, uint32_t mip_levels, bool cube_map,
                                               const Sources& sources, RenderUploadMode mode)
{
    GC_ASSERT(format != VK_FORMAT_UNDEFINED);
    GC_ASSERT(texel_size > 0);
    GC_ASSERT(width > 0 && height > 0 && mip_levels >= 1);
    GC_ASSERT((width >> (mip_levels - 1)) > 0 && (height >> (mip_levels - 1)) > 0);

    Upload upload{};
    upload.sources = sources;
    upload.format = format;
    upload.width = width;
    upload.height = height;
    upload.mip_levels = mip_levels;
    upload.cube_map = cube_map;
    upload.texel_size = texel_size;
    return startUpload(std::move(upload), mode);
}

RenderUpload RenderUploader::uploadBuffer(VkBuffer buffer, const Sources& sources, RenderUploadMode mode, VkDeviceSize offset)
{
    GC_ASSERT(buffer);

    Upload upload{};
    upload.sources = sources;
    upload.buffer = buffer;
    upload.buffer_offset = offset;
    return startUpload(std::move(upload), mode);
}

RenderUpload RenderUploader::startUpload(Upload&& upload, RenderUploadMode mode)
{
    GC_ASSERT(isMainThread());

    for (const std::span<const uint8_t> source : upload.sources) {
        upload.size += source.size();
    }
    GC_ASSERT(upload.size > 0);

    upload.state = std::make_shared<RenderUploadState>();
    upload.state->uploader = this;
    RenderUpload handle(upload.state);

    if (mode == RenderUploadMode::IMMEDIATE) {
        prepareUpload(upload);
        submit(std::span<Upload>(&upload, 1));
    }
    else {
        if (m_outstanding_count == 0) {
            m_burst_count = 0;
            m_burst_size = 0;
            m_burst_stopwatch.emplace();
        }
        ++m_outstanding_count;
        ++m_burst_count;
        m_burst_size += upload.size;
        {
            const std::lock_guard lock(m_mutex);
            m_queued.push_back(std::move(upload));
        }
        m_work_available.notify_one();
    }
    return handle;
}

void RenderUploader::runWorkThread()
{
    tracy::SetThreadName("Render Work Thread");

    std::unique_lock lock(m_mutex);
    for (;;) {
        m_work_available.wait(lock, [this]() { return m_shutdown || !m_queued.empty(); });
        if (m_shutdown) {
            return;
        }

        // The upload stays at the front of the queue while it is worked on, so that waitForSubmit() can see that it hasn't
        // finished. Only this thread takes uploads out of the queue, so it is still at the front afterwards.
        Upload& upload = m_queued.front();
        lock.unlock();
        if (!upload.state->cancelled.load()) {
            prepareUpload(upload);
        }
        lock.lock();
        m_finished.push_back(std::move(upload));
        m_queued.pop_front();
        m_work_finished.notify_all();
    }
}

// The parts of an upload that take the CPU's time. The data is read (from a memory mapped .gcpak file, usually, so this is also
// when it is read from the disk) and written to memory that the GPU can copy from, and the memory of an image is allocated.
// (The allocator, and making things with the device, can be used by several threads at once.)
void RenderUploader::prepareUpload(Upload& upload) const
{
    ZoneScoped;
    ZoneValue(upload.size);

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = upload.size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo alloc_create_info{};
    alloc_create_info.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_create_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc_create_info.priority = 0.5f;
    VmaAllocationInfo alloc_info{};
    GC_CHECKVK(vmaCreateBuffer(m_allocator, &buffer_info, &alloc_create_info, &upload.staging_buffer, &upload.staging_allocation, &alloc_info));
    GC_ASSERT(alloc_info.pMappedData);

    uint8_t* dest = static_cast<uint8_t*>(alloc_info.pMappedData);
    for (const std::span<const uint8_t> source : upload.sources) {
        std::memcpy(dest, source.data(), source.size());
        dest += source.size();
    }
    GC_CHECKVK(vmaFlushAllocation(m_allocator, upload.staging_allocation, 0, VK_WHOLE_SIZE));

    if (upload.isImage()) {
        std::tie(upload.image, upload.image_allocation) =
            vkutils::createImage(m_allocator, upload.format, upload.width, upload.height, upload.mip_levels, VK_SAMPLE_COUNT_1_BIT,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0.5f, false, upload.cube_map);
        upload.image_view = vkutils::createImageView(m_device, upload.image, upload.format, VK_IMAGE_ASPECT_COLOR_BIT, upload.mip_levels, upload.cube_map);
    }
}

// Destroys what prepareUpload() made, for an upload that the GPU was never told about (so none of it is in use)
void RenderUploader::destroyUnsubmitted(const Upload& upload) const
{
    if (upload.staging_buffer) {
        vmaDestroyBuffer(m_allocator, upload.staging_buffer, upload.staging_allocation);
    }
    if (upload.image) {
        vkDestroyImageView(m_device, upload.image_view, nullptr);
        vmaDestroyImage(m_allocator, upload.image, upload.image_allocation);
    }
}

void RenderUploader::submitFinishedUploads()
{
    GC_ASSERT(isMainThread());

    std::vector<Upload> finished{};
    {
        const std::lock_guard lock(m_mutex);
        finished.swap(m_finished);
    }
    if (!finished.empty()) {
        submit(finished);
        m_outstanding_count -= static_cast<uint32_t>(finished.size());
        if (m_outstanding_count == 0) {
            GC_DEBUG("The render work thread has prepared {} uploads ({}) in {}", m_burst_count, bytesToHumanReadable(m_burst_size), *m_burst_stopwatch);
        }
    }
}

void RenderUploader::waitForSubmit(const RenderUploadState& state)
{
    GC_ASSERT(isMainThread());

    if (state.submitted) {
        return;
    }
    {
        std::unique_lock lock(m_mutex);
        // (an upload that isn't queued any more has finished)
        m_work_finished.wait(lock, [&]() {
            return std::none_of(m_queued.begin(), m_queued.end(), [&](const Upload& upload) { return upload.state.get() == &state; });
        });
    }
    submitFinishedUploads();
}

// Tells the GPU to copy the staging buffers of all these uploads, with one command buffer. Uploads whose texture or mesh has
// been destroyed in the meantime are left out.
void RenderUploader::submit(std::span<Upload> uploads)
{
    ZoneScoped;

    const auto is_wanted = [](const Upload& upload) { return upload.staging_buffer && !upload.state->cancelled.load(); };

    uint64_t signal_value{};
    if (std::any_of(uploads.begin(), uploads.end(), is_wanted)) {
        VkCommandBufferAllocateInfo cmd_info{};
        cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_info.commandPool = m_cmd_pool;
        cmd_info.commandBufferCount = 1;
        VkCommandBuffer cmd{};
        GC_CHECKVK(vkAllocateCommandBuffers(m_device, &cmd_info, &cmd));

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        GC_CHECKVK(vkBeginCommandBuffer(cmd, &begin_info));

        for (const Upload& upload : uploads) {
            if (!is_wanted(upload)) {
                continue;
            }
            if (upload.isImage()) {
                recordImageCopy(cmd, upload);
            }
            else {
                recordBufferCopy(cmd, upload);
            }
        }

        GC_CHECKVK(vkEndCommandBuffer(cmd));

        signal_value = ++m_timeline_value;

        VkCommandBufferSubmitInfo cmd_submit_info{};
        cmd_submit_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd_submit_info.commandBuffer = cmd;
        VkSemaphoreSubmitInfo signal_info{};
        signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_info.semaphore = m_timeline_semaphore;
        signal_info.value = signal_value;
        signal_info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit_info.waitSemaphoreInfoCount = 0;
        submit_info.commandBufferInfoCount = 1;
        submit_info.pCommandBufferInfos = &cmd_submit_info;
        submit_info.signalSemaphoreInfoCount = 1;
        submit_info.pSignalSemaphoreInfos = &signal_info;
        GC_CHECKVK(vkQueueSubmit2(m_transfer_queue, 1, &submit_info, VK_NULL_HANDLE));

        GPUResourceDeleteQueue::DeletionEntry command_buffer_deletion_entry{};
        command_buffer_deletion_entry.timeline_semaphore = m_timeline_semaphore;
        command_buffer_deletion_entry.resource_free_signal_value = signal_value;
        const VkCommandPool cmd_pool = m_cmd_pool;
        command_buffer_deletion_entry.deleter = [cmd_pool, cmd](VkDevice device, [[maybe_unused]] VmaAllocator allocator) {
            GC_TRACE("freeing command buffer: {}", reinterpret_cast<void*>(cmd));
            vkFreeCommandBuffers(device, cmd_pool, 1, &cmd);
        };
        m_delete_queue.markForDeletion(command_buffer_deletion_entry);
    }

    for (Upload& upload : uploads) {
        if (!is_wanted(upload)) {
            destroyUnsubmitted(upload);
            continue;
        }
        {
            // (destroyed, by the delete queue, once the GPU has copied it)
            GPUBuffer staging_buffer(m_delete_queue, upload.staging_buffer, upload.staging_allocation);
            staging_buffer.useResource(m_timeline_semaphore, signal_value);
        }
        upload.state->timeline_semaphore = m_timeline_semaphore;
        upload.state->signal_value = signal_value;
        upload.state->image = upload.image;
        upload.state->image_allocation = upload.image_allocation;
        upload.state->image_view = upload.image_view;
        upload.state->submitted = true;
    }
}

void RenderUploader::recordImageCopy(VkCommandBuffer cmd, const Upload& upload) const
{
    const uint32_t layer_count = upload.cube_map ? 6 : 1;

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.srcAccessMask = VK_ACCESS_2_NONE;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = m_queue_family_index;
    barrier.dstQueueFamilyIndex = m_queue_family_index;
    barrier.image = upload.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = layer_count;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;

    if (upload.texel_size > 0) {
        // Every mip level is in the staging buffer, one after another
        barrier.subresourceRange.levelCount = upload.mip_levels;
        vkCmdPipelineBarrier2(cmd, &dependency);

        std::vector<VkBufferImageCopy> regions(upload.mip_levels);
        VkDeviceSize offset = 0;
        for (uint32_t level = 0; level < upload.mip_levels; ++level) {
            VkBufferImageCopy& region = regions[level];
            region.bufferOffset = offset;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = level;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = layer_count;
            region.imageExtent.width = upload.width >> level;
            region.imageExtent.height = upload.height >> level;
            region.imageExtent.depth = 1;
            offset += static_cast<VkDeviceSize>(region.imageExtent.width) * region.imageExtent.height * layer_count * upload.texel_size;
        }
        GC_ASSERT(offset == upload.size);
        vkCmdCopyBufferToImage(cmd, upload.staging_buffer, upload.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()),
                               regions.data());

        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dependency);
        return;
    }

    vkCmdPipelineBarrier2(cmd, &dependency);

    // the layers are one after another in the staging buffer
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = layer_count;
    region.imageOffset.x = 0;
    region.imageOffset.y = 0;
    region.imageOffset.z = 0;
    region.imageExtent.width = upload.width;
    region.imageExtent.height = upload.height;
    region.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, upload.staging_buffer, upload.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    if (upload.mip_levels > 1) {
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        vkCmdPipelineBarrier2(cmd, &dependency);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.subresourceRange.baseMipLevel = 1;
        barrier.subresourceRange.levelCount = upload.mip_levels - 1;
        vkCmdPipelineBarrier2(cmd, &dependency);

        generateMipMaps(cmd, upload.image, upload.width, upload.height);
    }
    else {
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }
}

void RenderUploader::recordBufferCopy(VkCommandBuffer cmd, const Upload& upload) const
{
    VkBufferCopy region{};
    region.srcOffset = 0;
    region.dstOffset = upload.buffer_offset;
    region.size = upload.size;
    vkCmdCopyBuffer(cmd, upload.staging_buffer, upload.buffer, 1, &region);

    VkBufferMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    barrier.srcQueueFamilyIndex = m_queue_family_index;
    barrier.dstQueueFamilyIndex = m_queue_family_index;
    barrier.buffer = upload.buffer;
    barrier.offset = upload.buffer_offset;
    barrier.size = upload.size;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

} // namespace gc
