#pragma once

// Gets the data of textures and meshes into GPU memory, for the RenderBackend.
//
// An upload has two steps. The data is first copied into a staging buffer, which the CPU can write to. Then the GPU is told to
// copy the staging buffer into the image or buffer that is drawn with. The GPU's copy never held the game up, as it is only
// waited for by not drawing the thing yet. The CPU's part did: reading hundreds of megabytes out of a .gcpak file and into
// staging buffers takes most of a second, and allocating the GPU memory of a world's textures takes a good part of another
// tenth. It was all done when the texture or mesh was created, in the middle of a frame.
//
// So that part can be done on another thread, the render work thread: uploads are queued for it, and for each one, in the order
// they were asked for, it makes the staging buffer and fills it, and makes the image if the upload is a texture. Everything
// else stays on the main thread. Once a frame, the main thread takes the uploads that the work thread has finished with and
// submits the GPU copies for all of them at once (submitFinishedUploads()). Until then, the texture or mesh isn't uploaded
// (see RenderUpload), like one whose GPU copy hasn't finished, and the renderer leaves out whatever needs it.
//
// The work thread reads the data some time after the upload was asked for, so only data that can't go away is given to it:
// see RenderUploadMode.

#include <cstdint>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <gctemplates/gct_static_vector.h>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_gpu_resources.h"
#include "gamecore/gc_stopwatch.h"
#include "gamecore/gc_vulkan_common.h"

namespace gc {

// When the data that a texture or a mesh is created from is copied
enum class RenderUploadMode {
    // Before the create function returns, on the thread that called it. The data can be freed straight away.
    // Copying a lot of data holds up the frame.
    IMMEDIATE,
    // Later, on the render work thread. The data has to stay where it is, unchanged, until the RenderBackend is destroyed, even
    // if the texture or mesh is destroyed first. Assets in the Content do (see getRenderUploadMode() in
    // gc_render_texture_manager.h). The texture or mesh takes a little longer to be uploaded, and nothing is held up.
    BACKGROUND,
};

class RenderUploader; // forward-dec

// Where one upload has got to. Shared by the uploader and the texture or mesh that is being uploaded (which can be moved, or
// destroyed, while the upload is still going on).
struct RenderUploadState {
    // Set (from the main thread) when the thing being uploaded has been destroyed, so that nothing is copied into it
    std::atomic<bool> cancelled{false};

    // The rest is only for the main thread.
    RenderUploader* uploader{};
    // Whether the GPU has been told to copy the staging buffer. If so, the copy is finished when the semaphore reaches the value
    bool submitted{false};
    VkSemaphore timeline_semaphore{};
    uint64_t signal_value{};
    // The image that was made for an image upload, once it has been submitted. Whoever takes these has to destroy them
    VkImage image{};
    VmaAllocation image_allocation{};
    VkImageView image_view{};
};

// What a RenderTexture or a RenderMesh keeps of its upload. Main thread only.
class RenderUpload {
    std::shared_ptr<RenderUploadState> m_state{}; // null once the resource has been told about the GPU's copy

public:
    RenderUpload() = default;
    explicit RenderUpload(std::shared_ptr<RenderUploadState> state) : m_state(std::move(state)) {}
    RenderUpload(const RenderUpload&) = delete;
    RenderUpload(RenderUpload&&) noexcept = default;

    ~RenderUpload()
    {
        if (m_state) {
            GC_ASSERT(!m_state->submitted); // otherwise the GPU may still be copying into something that is being destroyed
            m_state->cancelled.store(true);
        }
    }

    RenderUpload& operator=(const RenderUpload&) = delete;
    RenderUpload& operator=(RenderUpload&&) = delete;

    // False while the render work thread still has the upload, or hasn't got to it yet: the GPU hasn't been given anything to
    // do. Once this is true, finishBuffer() or takeTexture() must be called before this object is destroyed.
    bool isSubmitted() const { return !m_state || m_state->submitted; }

    // Whether there is nothing left to do with the upload: it was finished with, or moved to another object
    bool isDone() const { return !m_state; }

    // Blocks until isSubmitted()
    void waitForSubmit();

    // For a buffer upload that has been submitted: makes the buffer be in use until the GPU has finished copying into it.
    // Does nothing if that was already done.
    void finishBuffer(GPUResource& buffer)
    {
        if (m_state) {
            GC_ASSERT(m_state->submitted);
            buffer.useResource(m_state->timeline_semaphore, m_state->signal_value);
            m_state.reset();
        }
    }

    // For an image upload that has been submitted: the image, which is in use until the GPU has finished copying into it.
    // Can only be called once.
    GPUTexture takeTexture();
};

class RenderUploader {
public:
    // What an upload copies from: one or more pieces of data, which go into the staging buffer one after another
    using Sources = gct::static_vector<std::span<const uint8_t>, 6>;

private:
    struct Upload {
        std::shared_ptr<RenderUploadState> state{};
        Sources sources{};
        VkDeviceSize size{}; // of all the sources

        // Where the GPU copies the staging buffer to. For an image upload, an image that is made for it:
        VkFormat format{VK_FORMAT_UNDEFINED};
        uint32_t width{};
        uint32_t height{};
        uint32_t mip_levels{};
        bool cube_map{};
        // Zero if the data is only the first mip level, and the others are generated from it. Otherwise the data is every mip
        // level, largest first, and this is the size of a texel in bytes
        uint32_t texel_size{};
        // Otherwise a buffer that exists already, and where in it the data goes:
        VkBuffer buffer{};
        VkDeviceSize buffer_offset{};

        // made on the work thread (or by the caller, for RenderUploadMode::IMMEDIATE)
        VkBuffer staging_buffer{};
        VmaAllocation staging_allocation{};
        VkImage image{};
        VmaAllocation image_allocation{};
        VkImageView image_view{};

        bool isImage() const { return format != VK_FORMAT_UNDEFINED; }
    };

    const VkDevice m_device;
    const VmaAllocator m_allocator;
    GPUResourceDeleteQueue& m_delete_queue;
    const VkQueue m_transfer_queue;
    const uint32_t m_queue_family_index;
    const VkSemaphore m_timeline_semaphore; // signalled by the transfer queue when a batch of copies has finished
    uint64_t m_timeline_value{};
    VkCommandPool m_cmd_pool{};

    // For the log: what the work thread has been given since it last had nothing to do
    uint32_t m_outstanding_count{}; // not submitted yet
    uint32_t m_burst_count{};
    VkDeviceSize m_burst_size{};
    std::optional<Stopwatch> m_burst_stopwatch{};

    // Shared with the work thread, under the mutex
    std::mutex m_mutex{};
    std::condition_variable m_work_available{}; // for the work thread
    std::condition_variable m_work_finished{};  // for a main thread that is waiting for an upload
    std::deque<Upload> m_queued{};
    std::vector<Upload> m_finished{}; // the staging buffer is full. Waiting for submitFinishedUploads()
    bool m_shutdown{false};

    std::thread m_thread{};

public:
    // The semaphore is the transfer queue's timeline semaphore. Nothing else may signal it. Starts the render work thread
    RenderUploader(VkDevice device, VmaAllocator allocator, GPUResourceDeleteQueue& delete_queue, VkQueue transfer_queue, uint32_t queue_family_index,
                   VkSemaphore timeline_semaphore);
    RenderUploader(const RenderUploader&) = delete;

    // Stops the work thread. Uploads that weren't submitted are dropped
    ~RenderUploader();

    RenderUploader& operator=(const RenderUploader&) = delete;

    // The functions below are for the main thread.

    // Makes an image and uploads its first mip level. The other levels are generated from it. A cube map has six layers, and
    // the sources are the layers, or all of them in one piece. Mipmaps aren't generated for cube maps.
    // The image isn't made straight away (unless the mode is IMMEDIATE): get it with RenderUpload::takeTexture() once the
    // upload has been submitted. It will be in the layout SHADER_READ_ONLY_OPTIMAL.
    RenderUpload uploadImage(VkFormat format, uint32_t width, uint32_t height, uint32_t mip_levels, bool cube_map, const Sources& sources,
                             RenderUploadMode mode);

    // The same, for an image whose data has every mip level in it, largest first (and for a cube map, each level's six faces
    // together). Each level is half the size of the one before. texel_size is how many bytes a texel is in this format.
    RenderUpload uploadImageLevels(VkFormat format, uint32_t texel_size, uint32_t width, uint32_t height, uint32_t mip_levels, bool cube_map,
                                   const Sources& sources, RenderUploadMode mode);

    // Uploads to a buffer, from 'offset' on, which ends up ready to be read as vertices and indices, or by vertex shaders as a
    // storage buffer. The buffer can be in use by the GPU elsewhere, as long as nothing is reading or writing this part of it.
    RenderUpload uploadBuffer(VkBuffer buffer, const Sources& sources, RenderUploadMode mode, VkDeviceSize offset = 0);

    // Tells the GPU to copy every staging buffer that the work thread has filled since this was last called. Call once a frame.
    void submitFinishedUploads();

    // Blocks until the work thread has finished with the upload, then submits it (with any others that have finished)
    void waitForSubmit(const RenderUploadState& state);

    GPUResourceDeleteQueue& getDeleteQueue() const { return m_delete_queue; }

private:
    RenderUpload startUpload(Upload&& upload, RenderUploadMode mode);
    void runWorkThread();
    void prepareUpload(Upload& upload) const; // any thread
    void destroyUnsubmitted(const Upload& upload) const;
    void submit(std::span<Upload> uploads);
    void recordImageCopy(VkCommandBuffer cmd, const Upload& upload) const;
    void recordBufferCopy(VkCommandBuffer cmd, const Upload& upload) const;
};

} // namespace gc
